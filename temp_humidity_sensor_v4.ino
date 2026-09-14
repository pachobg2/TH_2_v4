/*
 * ESP32-C3 Battery-Powered Temp/Humidity Sensor (SHTC3) -- v4
 * Fork of temp_humidity_sensor with a self-service web setup portal
 * instead of a per-device compiled config.h. SHTC3 (I2C) + battery
 * voltage -> MQTT -> Home Assistant (via MQTT discovery).
 *
 * Wiring (same as temp_humidity_sensor):
 *   SHTC3: SDA -> GPIO4, SCL -> GPIO5
 *   Sensor power switch (MOSFET/load switch gate): GPIO3
 *   Status LED: GPIO7
 *   Battery voltage divider: midpoint -> GPIO1 (ADC)
 *   Setup/OTA button: one leg -> GPIO0, other leg -> GND (INPUT_PULLUP,
 *     active LOW). An external ~10k pull-up from GPIO0 to 3.3V is
 *     recommended alongside the internal one -- deep-sleep GPIO wakeup on
 *     the ESP32-C3 needs a reliably-held HIGH level while idle, and
 *     internal pulls aren't always dependable across sleep the way an
 *     external resistor is.
 *
 * What's different from temp_humidity_sensor:
 *   - No compiled-in WiFi/MQTT/device-identity secrets. On first-ever boot
 *     (or whenever the setup button is held at boot), the device broadcasts
 *     its own WiFi network ("TempSensorV4-XXXXXX"), scans nearby networks,
 *     and serves a small web page (WiFiManager) where you pick your WiFi
 *     and enter your MQTT broker host/port/user/password and a device
 *     name -- no per-device config.h edit or re-flash needed to deploy a
 *     new unit, just power it on and configure it from your phone.
 *   - Settings are saved to flash (NVS via Preferences), not compiled in,
 *     so the exact same firmware binary works on every unit.
 *   - The setup button is now hold-duration sensitive: released quickly
 *     (under BUTTON_OTA_HOLD_MS) is just a normal cycle, same as no press
 *     at all. Held 2-10s opens a button-triggered OTA-only window (no
 *     portal, LED solid on) -- same idea as the remote MQTT "OTA Request"
 *     switch, just triggered locally. Held past 10s opens the full setup
 *     portal (LED blinking once a second) -- once it succeeds, it opens a
 *     normal ArduinoOTA window too before restarting into normal
 *     operation. An unconfigured device always goes straight to the
 *     portal regardless of hold duration, since the OTA-only path needs
 *     already-saved WiFi credentials that don't exist yet. Remote OTA via
 *     the "OTA Request" MQTT switch is unchanged from temp_humidity_sensor
 *     and doesn't go through any of this at all.
 *   - Device ID defaults to an auto-generated, stable "th4_XXXXXX" (from
 *     the chip's own MAC) so units never collide on MQTT topics out of the
 *     box, but you can override it in the portal if you want a memorable
 *     topic name instead.
 *
 * Everything else -- sensor read, battery curve, LED feedback, boot/fail
 * counters, last-full-charge tracking, HA discovery, deep sleep -- is
 * unchanged from temp_humidity_sensor.
 *
 * NOT YET FLASHED TO REAL HARDWARE. WiFiManager's exact API has shifted
 * across versions; double-check WiFiManagerParameter/autoConnect/
 * startConfigPortal signatures against whatever version Library Manager
 * installs for you, and treat the first bring-up like any new sketch in
 * this fleet -- watch the Serial Monitor across a full boot.
 *
 * Libraries required (Library Manager):
 *   - espMqttClient (bertmelis) — QoS 1 publish with broker PUBACK confirmation
 *   - Adafruit SHTC3 (+ Adafruit BusIO, Adafruit Unified Sensor)
 *   - WiFiManager (tzapu) — the setup portal
 *   - ArduinoOTA, Preferences (bundled with the ESP32 core)
 *   - AsyncTCP (ESP32Async) — not used directly, but espMqttClient's
 *     source tree includes an async transport that always gets compiled;
 *     without this installed you'll hit a missing-header build error even
 *     though nothing here calls into it (same gotcha as toshiba_ac_bridge)
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <espMqttClient.h>
#include <Wire.h>
#include <Adafruit_SHTC3.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp32-hal-bt.h"
#include <time.h>
#include <sys/time.h>
#include <vector>
#include "config.h"

// Defined up here, right after the includes, rather than down by the
// functions that use it -- the Arduino IDE auto-generates prototypes for
// any function lacking one and inserts them near the top of the file, so a
// custom type used in such a prototype has to already be visible by this
// point or the auto-generated prototype fails to compile.
enum ButtonHoldMode { BUTTON_HOLD_NONE, BUTTON_HOLD_OTA, BUTTON_HOLD_SETUP };

// ---------------- Runtime settings (NVS, not compiled in) ----------------
// WiFi, MQTT, and device identity used to live in config.h across this
// fleet; here they're set once via the setup portal and persisted to
// flash instead, so the same firmware image works on every unit.

struct Settings {
  String wifiSsid;
  String wifiPassword;
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUser;
  String mqttPassword;
  String deviceId;   // used in MQTT topics/unique_ids -- keep stable once devices exist
  String deviceName; // friendly name shown in Home Assistant
  bool configured = false;
};

Settings settings;
Preferences settingsPrefs;

String getShortChipId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[7];
  snprintf(buf, sizeof(buf), "%06X", (uint32_t)(mac & 0xFFFFFFULL));
  return String(buf);
}

void loadSettings() {
  String chipId = getShortChipId();
  settingsPrefs.begin("settings", true); // read-only
  settings.configured   = settingsPrefs.getBool("configured", false);
  settings.wifiSsid     = settingsPrefs.getString("wifiSsid", "");
  settings.wifiPassword = settingsPrefs.getString("wifiPass", "");
  settings.mqttHost     = settingsPrefs.getString("mqttHost", "");
  settings.mqttPort     = settingsPrefs.getUShort("mqttPort", 1883);
  settings.mqttUser     = settingsPrefs.getString("mqttUser", "");
  settings.mqttPassword = settingsPrefs.getString("mqttPass", "");
  settings.deviceId     = settingsPrefs.getString("deviceId", "th4_" + chipId);
  settings.deviceName   = settingsPrefs.getString("deviceName", "Temp Sensor " + chipId);
  settingsPrefs.end();
}

void saveSettings() {
  settingsPrefs.begin("settings", false);
  settingsPrefs.putBool("configured", settings.configured);
  settingsPrefs.putString("wifiSsid", settings.wifiSsid);
  settingsPrefs.putString("wifiPass", settings.wifiPassword);
  settingsPrefs.putString("mqttHost", settings.mqttHost);
  settingsPrefs.putUShort("mqttPort", settings.mqttPort);
  settingsPrefs.putString("mqttUser", settings.mqttUser);
  settingsPrefs.putString("mqttPass", settings.mqttPassword);
  settingsPrefs.putString("deviceId", settings.deviceId);
  settingsPrefs.putString("deviceName", settings.deviceName);
  settingsPrefs.end();
}

// MQTT topics -- built once at the top of a normal-mode boot, after
// settings are loaded (deviceId isn't known at compile time here the way
// DEVICE_ID is in the rest of this fleet).
String TOPIC_TEMP, TOPIC_HUMIDITY, TOPIC_BATTERY_V, TOPIC_BATTERY_PCT, TOPIC_RSSI,
       TOPIC_LAST_UPDATE, TOPIC_RESET_REASON, TOPIC_FAIL_COUNT, TOPIC_TOTAL_FAIL_COUNT,
       TOPIC_OTA_REQUEST, TOPIC_BATTERY_LOW, TOPIC_BOOT_COUNT, TOPIC_FW_VERSION,
       TOPIC_LAST_FULL_CHARGE;
String DISCOVERY_TEMP, DISCOVERY_HUMIDITY, DISCOVERY_BATTERY_V, DISCOVERY_BATTERY_PCT,
       DISCOVERY_RSSI, DISCOVERY_LAST_UPDATE, DISCOVERY_RESET_REASON, DISCOVERY_FAIL_COUNT,
       DISCOVERY_TOTAL_FAIL_COUNT, DISCOVERY_OTA_REQUEST, DISCOVERY_BATTERY_LOW,
       DISCOVERY_BOOT_COUNT, DISCOVERY_FW_VERSION, DISCOVERY_LAST_FULL_CHARGE;

void buildTopics() {
  String base = String("home/") + settings.deviceId;
  TOPIC_TEMP             = base + "/temperature";
  TOPIC_HUMIDITY         = base + "/humidity";
  TOPIC_BATTERY_V        = base + "/battery_voltage";
  TOPIC_BATTERY_PCT      = base + "/battery_percent";
  TOPIC_RSSI             = base + "/wifi_signal";
  TOPIC_LAST_UPDATE      = base + "/last_update";
  TOPIC_RESET_REASON     = base + "/reset_reason";
  TOPIC_FAIL_COUNT       = base + "/connect_fail_count";
  TOPIC_TOTAL_FAIL_COUNT = base + "/total_fail_count";
  TOPIC_OTA_REQUEST      = base + "/ota_request"; // retained flag, set from HA to request OTA remotely
  TOPIC_BATTERY_LOW      = base + "/battery_low";
  TOPIC_BOOT_COUNT       = base + "/boot_count";
  TOPIC_FW_VERSION       = base + "/firmware_version";
  TOPIC_LAST_FULL_CHARGE = base + "/last_full_charge";

  String sbase = String("homeassistant/sensor/") + settings.deviceId;
  DISCOVERY_TEMP             = sbase + "/temperature/config";
  DISCOVERY_HUMIDITY         = sbase + "/humidity/config";
  DISCOVERY_BATTERY_V        = sbase + "/battery_voltage/config";
  DISCOVERY_BATTERY_PCT      = sbase + "/battery_percent/config";
  DISCOVERY_RSSI             = sbase + "/wifi_signal/config";
  DISCOVERY_LAST_UPDATE      = sbase + "/last_update/config";
  DISCOVERY_RESET_REASON     = sbase + "/reset_reason/config";
  DISCOVERY_FAIL_COUNT       = sbase + "/connect_fail_count/config";
  DISCOVERY_TOTAL_FAIL_COUNT = sbase + "/total_fail_count/config";
  DISCOVERY_BOOT_COUNT       = sbase + "/boot_count/config";
  DISCOVERY_FW_VERSION       = sbase + "/firmware_version/config";
  DISCOVERY_LAST_FULL_CHARGE = sbase + "/last_full_charge/config";
  DISCOVERY_BATTERY_LOW      = String("homeassistant/binary_sensor/") + settings.deviceId + "/battery_low/config";
  DISCOVERY_OTA_REQUEST      = String("homeassistant/switch/") + settings.deviceId + "/ota_request/config";
}

// ---------------- Persisted state (survives deep sleep) ----------------

RTC_DATA_ATTR bool discoverySent = false;
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint32_t connectFailCount = 0; // increments on any wake that fails to publish, resets on success
RTC_DATA_ATTR uint32_t totalFailCount = 0; // lifetime total failed wakes -- never resets, mirrors bootCount
RTC_DATA_ATTR uint8_t cachedWifiChannel = 0; // 0 = unknown yet, let WiFi.begin() auto-select
RTC_DATA_ATTR bool g_timeSynced = false; // true once any cycle has completed a real NTP sync

// ---------------- Globals ----------------

espMqttClient mqttClient;
Adafruit_SHTC3 shtc3 = Adafruit_SHTC3();
Preferences batteryPrefs; // flash/NVS, not RTC memory -- survives an actual battery depletion

// Tracks the PUBACK for whichever single publish is currently in flight.
// publishWithAck() only ever has one outstanding packet at a time, so a
// single tracked ID (rather than a list) is enough here.
volatile uint16_t g_trackedPacketId = 0;
volatile bool g_packetAcked = false;

void onMqttPublish(uint16_t packetId) {
  if (packetId == g_trackedPacketId) {
    g_packetAcked = true;
  }
}

// Remote OTA trigger: set from Home Assistant (a switch entity) rather than
// only via the physical button. Since the device is subscribed for only a
// moment each cycle, this relies on the message being retained -- the
// broker delivers it immediately on subscribe, no polling needed.
volatile bool g_otaRequestReceived = false;
char g_otaRequestPayload[8] = {0};

void onMqttMessage(const espMqttClientTypes::MessageProperties& properties, const char* topic,
                    const uint8_t* payload, size_t len, size_t index, size_t total) {
  if (TOPIC_OTA_REQUEST.equals(topic)) {
    size_t copyLen = len < sizeof(g_otaRequestPayload) - 1 ? len : sizeof(g_otaRequestPayload) - 1;
    memcpy(g_otaRequestPayload, payload, copyLen);
    g_otaRequestPayload[copyLen] = '\0';
    g_otaRequestReceived = true;
  }
}

// ---------------- Awake watchdog ----------------
// A hardware timer that force-restarts the device if it's ever awake too
// long -- a hang, an unexpected infinite loop, a stuck library call. This
// is independent of everything else in the sketch: even if the main flow
// gets stuck somewhere no one anticipated, this guarantees the device
// eventually resets rather than draining the battery all night awake.

esp_timer_handle_t g_watchdogTimer = nullptr;

void watchdogTimeoutHandler(void* arg) {
  esp_restart();
}

void startAwakeWatchdog(uint32_t timeoutMs) {
  if (g_watchdogTimer != nullptr) {
    esp_timer_stop(g_watchdogTimer);
    esp_timer_delete(g_watchdogTimer);
    g_watchdogTimer = nullptr;
  }
  esp_timer_create_args_t args = {};
  args.callback = &watchdogTimeoutHandler;
  args.arg = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "awake_wdt";
  esp_timer_create(&args, &g_watchdogTimer);
  esp_timer_start_once(g_watchdogTimer, (uint64_t)timeoutMs * 1000ULL);
}

void stopAwakeWatchdog() {
  if (g_watchdogTimer != nullptr) {
    esp_timer_stop(g_watchdogTimer);
    esp_timer_delete(g_watchdogTimer);
    g_watchdogTimer = nullptr;
  }
}

// ---------------- Setup button ----------------
// Three-way hold detection: released quickly (or not held at all) means a
// normal cycle; a deliberate 2-10s hold opens an OTA-only window; past 10s
// opens the full setup portal. See BUTTON_OTA_HOLD_MS/BUTTON_SETUP_HOLD_MS
// in config.h. (ButtonHoldMode itself is defined up near the includes --
// see the comment there.)

const char* buttonHoldModeToString(ButtonHoldMode mode) {
  switch (mode) {
    case BUTTON_HOLD_OTA:   return "2-10s (OTA)";
    case BUTTON_HOLD_SETUP: return ">10s (setup)";
    default:                return "none";
  }
}

// Measures how long the setup button is held at boot. If this boot was
// itself woken by the button (deep-sleep GPIO wake), it may already have
// been held for a moment before we get here -- that's fine, we just start
// the clock now rather than trying to account for that. Commits to
// BUTTON_HOLD_SETUP the instant the hold crosses BUTTON_SETUP_HOLD_MS,
// without waiting for release, so a long hold feels immediate rather than
// requiring you to guess when to let go.
ButtonHoldMode readButtonHoldMode() {
  if (digitalRead(SETUP_PIN) != LOW) return BUTTON_HOLD_NONE;

  unsigned long start = millis();
  while (digitalRead(SETUP_PIN) == LOW) {
    if (millis() - start >= BUTTON_SETUP_HOLD_MS) {
      return BUTTON_HOLD_SETUP;
    }
    delay(20);
  }

  unsigned long heldMs = millis() - start;
  return (heldMs >= BUTTON_OTA_HOLD_MS) ? BUTTON_HOLD_OTA : BUTTON_HOLD_NONE;
}

// ---------------- Function declarations ----------------

ButtonHoldMode readButtonHoldMode();
const char* buttonHoldModeToString(ButtonHoldMode mode);
void connectWiFi();
bool attemptWifiConnect(uint8_t channel);
bool connectMQTT();
bool publishWithAck(const char* topic, const char* payload, bool retained);
void sendDiscoveryConfig();
int publishState(float tempC, float humidity, float battV, float battPct, int rssi, const String& resetReasonStr, uint32_t failCount);
void goToSleep();
float readBatteryVoltage();
float batteryPercentage(float v);
void blink(int times, uint32_t onMs, uint32_t gapMs);
void runOtaWindow();
void enterOtaMode();
void runButtonOtaMode();
void runMaintenanceMode(bool viaButton);
bool syncTimeUtc();
String getCurrentTimestampUtc();
String resetReasonToString(esp_reset_reason_t reason);
String updateAndGetLastFullChargeDate(float batteryPercent);

// ---------------- Setup / main flow ----------------

void setup() {
  Serial.begin(115200);
  startAwakeWatchdog(AWAKE_WATCHDOG_TIMEOUT_MS); // armed immediately -- extended later if maintenance/OTA mode is entered

  btStop(); // BT controller isn't used here; costs nothing to make sure it's off

  if (DEBUG_MODE) {
    delay(DEBUG_BOOT_DELAY_MS);
    Serial.println("=== DEBUG_MODE is ON: deep sleep disabled, staying awake ===");
  } else {
    delay(100);
  }

  bootCount++;

  esp_reset_reason_t resetReason = esp_reset_reason();
  String resetReasonStr = resetReasonToString(resetReason);
  if (resetReason == ESP_RST_BROWNOUT) {
    connectFailCount++; // a brownout mid-cycle means this wake never got to publish either
    totalFailCount++;
  }

  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW); // ALWAYS_OFF until we explicitly power it
  ledcAttach(LED_PIN, LED_PWM_FREQ_HZ, LED_PWM_RESOLUTION);
  ledcWrite(LED_PIN, 0);
  pinMode(SETUP_PIN, INPUT_PULLUP);
  analogSetPinAttenuation((uint8_t)BATT_PIN, ADC_11db);

  loadSettings();

  // Check this as early as possible, before any slow work (sensor reads,
  // battery averaging). readButtonHoldMode() blocks for as long as the
  // button is actually held (up to BUTTON_SETUP_HOLD_MS), so this is also
  // where that hold time gets spent. An unconfigured device (no saved WiFi
  // yet) always goes straight to the portal, regardless of hold duration --
  // the OTA-only path below needs already-saved credentials it doesn't have.
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  ButtonHoldMode buttonMode = readButtonHoldMode();
  bool forceSetup = !settings.configured;
  Serial.printf("Boot #%lu, wakeup cause: %d, button hold: %s, configured: %s\n",
                bootCount, (int)wakeupCause, buttonHoldModeToString(buttonMode),
                settings.configured ? "yes" : "no");

  if (forceSetup || buttonMode == BUTTON_HOLD_SETUP) {
    runMaintenanceMode(buttonMode == BUTTON_HOLD_SETUP);
    // Only reached if the portal timed out / failed to connect -- a
    // successful run restarts the device itself and never returns here.

    if (DEBUG_MODE) {
      stopAwakeWatchdog();
      Serial.println("=== DEBUG_MODE: staying connected, entering loop() ===");
      return;
    }

    WiFi.disconnect(true);
    stopAwakeWatchdog();
    goToSleep();
    return;
  }

  if (buttonMode == BUTTON_HOLD_OTA) {
    runButtonOtaMode();

    if (DEBUG_MODE) {
      stopAwakeWatchdog();
      Serial.println("=== DEBUG_MODE: staying connected, entering loop() ===");
      return;
    }

    WiFi.disconnect(true);
    stopAwakeWatchdog();
    goToSleep();
    return;
  }

  buildTopics();

  // Power on SHTC3 and let it settle
  digitalWrite(SENSOR_POWER_PIN, HIGH);
  delay(SENSOR_POWER_SETTLE_MS);

  Wire.begin(SDA_PIN, SCL_PIN);
  bool shtOk = shtc3.begin(&Wire);
  float tempC = NAN, humidity = NAN;
  if (shtOk) {
    sensors_event_t humEvent, tempEvent;
    shtc3.getEvent(&humEvent, &tempEvent);
    tempC = tempEvent.temperature + TEMP_OFFSET_C;
    humidity = humEvent.relative_humidity;
  } else {
    Serial.println("SHTC3 not found on I2C bus!");
  }

  float battV = readBatteryVoltage();
  float battPct = batteryPercentage(battV);

  connectWiFi();

  bool published = false;
  bool otaModeEntered = false;
  int rssi = 0;

  if (WiFi.status() == WL_CONNECTED) {
    rssi = WiFi.RSSI();

    if (connectMQTT()) {
      // Subscribe now -- retained messages get delivered right away, and by
      // the time publishState() below finishes, there's been enough time
      // for the library's background task to have received and processed
      // it, with no extra blocking wait added on our part.
      g_otaRequestReceived = false;
      mqttClient.subscribe(TOPIC_OTA_REQUEST.c_str(), 1);

      if (!discoverySent) {
        sendDiscoveryConfig();
        discoverySent = true;
      }
      int unackedTopics = publishState(tempC, humidity, battV, battPct, rssi, resetReasonStr, connectFailCount);

      if (unackedTopics == 0) {
        published = true;
        connectFailCount = 0; // every topic confirmed by the broker, counter clears
      } else {
        Serial.printf("%d topic(s) never got a PUBACK this cycle.\n", unackedTopics);
        connectFailCount++;
        totalFailCount++;
      }

      bool remoteOtaRequested = g_otaRequestReceived && strcmp(g_otaRequestPayload, "ON") == 0;
      if (remoteOtaRequested) {
        Serial.println("Remote OTA request received via MQTT.");
        publishWithAck(TOPIC_OTA_REQUEST.c_str(), "OFF", true); // clear the flag so it doesn't retrigger next wake
        enterOtaMode();
        otaModeEntered = true;
      }
    } else {
      Serial.println("MQTT connect failed, skipping publish this cycle.");
      connectFailCount++;
      totalFailCount++;
    }
  } else {
    Serial.println("WiFi connect failed, skipping publish this cycle.");
    connectFailCount++;
    totalFailCount++;
  }

  // LED feedback: one short blink on success, three quick blinks on failure.
  // Skipped if we just ran the OTA flourish blink instead.
  if (!otaModeEntered) {
    if (published) {
      blink(1, 20, 200);
    } else {
      blink(3, 20, 200);
    }
  }

  digitalWrite(SENSOR_POWER_PIN, LOW);

  if (DEBUG_MODE) {
    stopAwakeWatchdog(); // staying awake indefinitely is intentional in debug mode
    Serial.println("=== DEBUG_MODE: staying connected, entering loop() ===");
    return;
  }

  mqttClient.disconnect();
  delay(100);
  WiFi.disconnect(true);

  stopAwakeWatchdog(); // about to sleep on our own terms, no need for the failsafe to fire mid-sleep
  goToSleep();
}

void loop() {
  if (DEBUG_MODE) {
    delay(1000); // espMqttClient runs its own background task, nothing to pump here
  }
  // In normal (non-debug) operation this is never reached — device sleeps at the end of setup()
}

// ---------------- WiFi ----------------

// Single connection attempt on the given channel (0 = let the radio auto-scan/pick).
// Returns true if connected within WIFI_CONNECT_TIMEOUT_MS.
bool attemptWifiConnect(uint8_t channel) {
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str(), channel);

  unsigned long start = millis();
  wl_status_t lastStatus = WiFi.status();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(50);
    wl_status_t s = WiFi.status();
    if (s != lastStatus) {
      Serial.printf("[debug] WiFi status changed: %d\n", s);
      lastStatus = s;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected in %lums, IP: %s, channel: %u\n",
                  millis() - start, WiFi.localIP().toString().c_str(), WiFi.channel());
    return true;
  }

  Serial.printf("[debug] Final WiFi status: %d (0=IDLE,1=NO_SSID,3=CONNECTED,4=CONNECT_FAILED,6=DISCONNECTED)\n",
                WiFi.status());
  return false;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // disable modem power-save during connect -- noticeably faster/more reliable, especially right out of deep sleep

  // Reusing the channel from the last successful connect skips a small
  // amount of scan/negotiation time. cachedWifiChannel is 0 (auto) until
  // the first successful connect populates it below.
  bool connected = attemptWifiConnect(cachedWifiChannel);

  // If the cached channel attempt failed (e.g. the router switched channels
  // since we last connected), fall back to one auto-scan retry before
  // giving up on this cycle entirely.
  if (!connected && cachedWifiChannel != 0) {
    Serial.println("[debug] Cached-channel connect failed, retrying with auto channel scan...");
    WiFi.disconnect();
    delay(100);
    connected = attemptWifiConnect(0);
  }

  cachedWifiChannel = connected ? WiFi.channel() : 0;
}

// ---------------- Setup portal ----------------

// Entered when the setup button is held at boot, or when this device has
// never been configured yet (settings.configured == false). Broadcasts
// "TempSensorV4-XXXXXX" immediately (no attempt to reconnect with any
// existing WiFi credentials first -- see the comment above the
// startConfigPortal() call for why), scans for nearby networks, and serves
// a page (WiFiManager) with a WiFi picker plus custom fields for MQTT and
// device identity. If the portal succeeds, settings are saved and the
// device restarts straight into normal operation -- to also push new
// firmware in the same visit, hold the button 2-10s on the next boot
// instead (runButtonOtaMode()). If the portal times out or is cancelled,
// this returns and the caller goes back to sleep with whatever settings
// already existed (unchanged).
void runMaintenanceMode(bool viaButton) {
  Serial.println(viaButton
    ? "Setup button held >10s -- entering maintenance mode."
    : "No saved WiFi config yet -- entering first-time setup.");
  startAwakeWatchdog((PORTAL_TIMEOUT_SEC + 60) * 1000UL);

  char mqttPortStr[6];
  snprintf(mqttPortStr, sizeof(mqttPortStr), "%u", settings.mqttPort);

  WiFiManagerParameter p_mqtt_host("mqtt_host", "MQTT broker host or IP", settings.mqttHost.c_str(), 64);
  WiFiManagerParameter p_mqtt_port("mqtt_port", "MQTT broker port", mqttPortStr, 6);
  WiFiManagerParameter p_mqtt_user("mqtt_user", "MQTT username", settings.mqttUser.c_str(), 32);
  WiFiManagerParameter p_mqtt_pass("mqtt_pass", "MQTT password", settings.mqttPassword.c_str(), 32, "type='password'");
  WiFiManagerParameter p_device_name("device_name", "Device name (shown in Home Assistant)", settings.deviceName.c_str(), 40);
  WiFiManagerParameter p_device_id("device_id", "Device ID (MQTT topics, no spaces)", settings.deviceId.c_str(), 32);
  // Checkbox via WiFiManager's custom-attribute trick (it has no native
  // checkbox type): submitted as "1" when checked, absent entirely
  // (getValue() == "") when not. The parameter's own default value is left
  // "" (not "1") deliberately -- "1" is only injected into the rendered
  // HTML's value= attribute via the custom-attribute string below, so it's
  // only what gets POSTed if the box is actually checked, not what
  // getValue() returns before any submission. A real factory reset --
  // wipes this project's own saved settings AND the ESP32 radio's own
  // persisted WiFi credentials -- unlike the portal's built-in "Erase"
  // menu button, which only clears the radio's WiFi credentials and
  // leaves our settings (including whatever's in the fields above)
  // untouched. That default button is hidden below (see setMenu()) to
  // avoid the two being confused for each other.
  WiFiManagerParameter p_factory_reset("factory_reset",
    "Factory reset (erase ALL saved settings, including WiFi)", "", 2, "type=\"checkbox\" value=\"1\"");

  WiFiManager wm;
  wm.addParameter(&p_mqtt_host);
  wm.addParameter(&p_mqtt_port);
  wm.addParameter(&p_mqtt_user);
  wm.addParameter(&p_mqtt_pass);
  wm.addParameter(&p_device_name);
  wm.addParameter(&p_device_id);
  wm.addParameter(&p_factory_reset);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_SEC);
  // Hide the built-in "Erase" menu button -- it only clears the radio's own
  // WiFi credentials, not our settings, which reads as a half-working
  // factory reset. Use the checkbox above instead for an actual full reset.
  std::vector<const char*> menu = {"wifi", "param", "info", "sep", "restart", "exit"};
  wm.setMenu(menu);

  // WPA2 requires an 8-63 character password -- anything shorter and
  // WiFi.softAP() fails to bring the AP up at all (no visible error, it
  // just never appears). Rather than fail silently on a bad config.h
  // value, fall back to an open setup network instead.
  const char* apPassword = AP_PASSWORD;
  size_t apPasswordLen = strlen(apPassword);
  if (apPasswordLen > 0 && apPasswordLen < 8) {
    Serial.printf("[setup] AP_PASSWORD is %u characters -- WPA2 needs at least 8, "
                  "falling back to an OPEN setup network instead of failing silently.\n",
                  (unsigned)apPasswordLen);
    apPassword = nullptr; // WiFiManager treats null as "open network, no password"
  }

  // Always startConfigPortal(), never autoConnect(): this function is only
  // ever reached when settings.configured is false (i.e. our own app-level
  // setup has never completed) or the button was held. Either way there's
  // no known-good config worth trying first -- autoConnect()'s "try the
  // radio's own last-saved network" step doesn't read from our settings at
  // all, it reads whatever the ESP32 WiFi driver itself last connected to
  // (persisted independently, chip-wide, by any firmware ever flashed to
  // this board). On a reused dev board that's often a stale network from a
  // completely different project, costing a real ~60s connect-timeout wait
  // before falling back to the portal, for a "saved" network we never
  // actually saved.
  // Non-blocking so the LED can blink for the duration of the portal
  // instead of sitting solid -- WiFiManager hands control back to us via
  // process(), rather than blocking inside startConfigPortal() itself.
  String apName = "TempSensorV4-" + getShortChipId();
  wm.setConfigPortalBlocking(false);
  wm.startConfigPortal(apName.c_str(), apPassword);

  unsigned long lastBlink = 0;
  bool ledOn = false;
  while (wm.getConfigPortalActive() && WiFi.status() != WL_CONNECTED) {
    wm.process();
    unsigned long now = millis();
    if (now - lastBlink >= SETUP_LED_BLINK_MS) {
      lastBlink = now;
      ledOn = !ledOn;
      ledcWrite(LED_PIN, ledOn ? ((uint32_t)LED_BRIGHTNESS_PCT * LED_PWM_MAX_DUTY) / 100 : 0);
    }
    delay(10);
  }
  bool connected = (WiFi.status() == WL_CONNECTED);

  // Checked before the connected/not-connected branch below, and
  // regardless of its outcome -- a factory reset doesn't need a live WiFi
  // connection to execute (it only touches flash), and gating it behind a
  // successful connect meant checking the box and hitting Save silently
  // did nothing whenever the WiFi fields weren't (re-)filled in too, e.g.
  // WiFiManager never pre-fills the WiFi password field on this page, so a
  // save with it left blank fails to connect on its own, independent of
  // the checkbox. The parameter's own default is "" (not "1"), so this
  // only fires on an actual submission with the box checked -- a plain
  // portal timeout leaves it unset.
  if (strcmp(p_factory_reset.getValue(), "1") == 0) {
    Serial.println("Factory reset requested from setup portal -- wiping saved settings and WiFi credentials.");
    settingsPrefs.begin("settings", false);
    settingsPrefs.clear();
    settingsPrefs.end();
    WiFi.disconnect(true, true); // also erase the radio's own persisted WiFi credentials
    blink(5, 50, 100);
    ledcWrite(LED_PIN, 0);
    stopAwakeWatchdog();
    Serial.println("Restarting into unconfigured state...");
    Serial.flush();
    delay(200);
    ESP.restart();
  }

  if (!connected) {
    Serial.println("Setup portal timed out / no connection -- resuming normal cycle with existing settings.");
    ledcWrite(LED_PIN, 0);
    blink(3, 20, 200);
    stopAwakeWatchdog();
    return;
  }

  settings.mqttHost     = p_mqtt_host.getValue();
  int parsedPort        = atoi(p_mqtt_port.getValue());
  settings.mqttPort     = (parsedPort > 0 && parsedPort <= 65535) ? (uint16_t)parsedPort : 1883;
  settings.mqttUser     = p_mqtt_user.getValue();
  settings.mqttPassword = p_mqtt_pass.getValue();
  settings.deviceName   = p_device_name.getValue();
  settings.deviceId     = p_device_id.getValue();
  settings.deviceId.replace(" ", "_"); // MQTT topics can't contain spaces
  // WiFiManager already connected us using whatever credentials it saved
  // (freshly entered in the portal, or previously-saved ones on a silent
  // autoConnect) -- capture them from the live connection rather than
  // re-parsing the portal's own internal state.
  settings.wifiSsid     = WiFi.SSID();
  settings.wifiPassword = WiFi.psk();

  // WiFi connected fine, but an empty MQTT host means this device could
  // never actually publish anything -- don't mark it "configured" on a
  // submission like that, or it silently gets stuck: nothing works, and
  // nothing prompts you back into the portal short of holding the button
  // for another 10s. Leaving `configured` false means the next boot goes
  // straight back to setup on its own, no button needed.
  if (settings.mqttHost.length() == 0) {
    saveSettings(); // still keep the WiFi/device fields that were filled in
    Serial.println("Setup portal closed with an empty MQTT broker host -- not marking as configured.");
    ledcWrite(LED_PIN, 0);
    blink(5, 20, 100); // distinct from the 3-blink connect-failure pattern
    stopAwakeWatchdog();
    return;
  }

  settings.configured   = true;
  saveSettings();
  Serial.printf("Setup saved: device_id=%s mqtt=%s:%u\n",
                settings.deviceId.c_str(), settings.mqttHost.c_str(), settings.mqttPort);

  // No OTA window here -- if firmware needs pushing too, hold the button
  // 2-10s on the next boot for that (runButtonOtaMode()). Restart straight
  // into normal operation instead of making every provisioning visit wait
  // out an unused OTA window.
  blink(1, 50, 50);
  ledcWrite(LED_PIN, 0);
  stopAwakeWatchdog();
  Serial.println("Restarting into normal operation...");
  Serial.flush();
  delay(200);
  ESP.restart();
}

// Setup button held 2-10s: a normal ArduinoOTA window, no portal --
// connects with the already-saved WiFi credentials. Unreachable on an
// unconfigured device (see forceSetup in setup()), since there'd be
// nothing to connect with. LED solid on for the duration, same as the
// remote-MQTT-triggered path in enterOtaMode().
void runButtonOtaMode() {
  Serial.println("Setup button held 2-10s -- entering OTA-only mode (no portal).");
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    enterOtaMode();
  } else {
    Serial.println("OTA button held but WiFi failed to connect -- going back to sleep.");
    blink(3, 20, 200);
  }
}

// ---------------- MQTT ----------------

bool connectMQTT() {
  mqttClient.setServer(settings.mqttHost.c_str(), settings.mqttPort);
  mqttClient.setCredentials(settings.mqttUser.c_str(), settings.mqttPassword.c_str());
  mqttClient.setClientId(settings.deviceId.c_str());
  mqttClient.onPublish(onMqttPublish);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.connect(); // non-blocking on ESP32 -- offloaded to the library's own task

  unsigned long start = millis();
  while (!mqttClient.connected() && millis() - start < MQTT_CONNECT_TIMEOUT_MS) {
    delay(50);
  }

  if (mqttClient.connected()) {
    Serial.printf("MQTT connected in %lums\n", millis() - start);
    return true;
  }

  Serial.println("MQTT connect failed/timed out");
  return false;
}

// Publishes one topic at QoS 1 and waits for the broker's PUBACK before
// returning. Retries up to MQTT_PUBLISH_RETRIES times if the ack doesn't
// arrive in time (dropped packet, broker hiccup, etc.) or if the library
// fails to even queue the publish (packetId 0, e.g. internal queue full).
bool publishWithAck(const char* topic, const char* payload, bool retained) {
  for (uint8_t attempt = 1; attempt <= MQTT_PUBLISH_RETRIES; attempt++) {
    g_packetAcked = false;
    g_trackedPacketId = 0;

    uint16_t packetId = mqttClient.publish(topic, 1 /* QoS 1 */, retained, payload);
    if (packetId == 0) {
      Serial.printf("[mqtt] queue failed for %s (attempt %u/%u)\n", topic, attempt, MQTT_PUBLISH_RETRIES);
      delay(150);
      continue;
    }
    g_trackedPacketId = packetId;

    unsigned long waitStart = millis();
    while (!g_packetAcked && millis() - waitStart < MQTT_ACK_TIMEOUT_MS) {
      delay(10);
    }

    if (g_packetAcked) {
      return true;
    }
    Serial.printf("[mqtt] no PUBACK for %s (packetId %u, attempt %u/%u)\n",
                  topic, packetId, attempt, MQTT_PUBLISH_RETRIES);
  }

  Serial.printf("[mqtt] giving up on %s after %u attempts\n", topic, MQTT_PUBLISH_RETRIES);
  return false;
}

void sendDiscoveryConfig() {
  String devBlock = String("\"device\":{\"identifiers\":[\"") + settings.deviceId
      + "\"],\"name\":\"" + settings.deviceName
      + "\",\"manufacturer\":\"" + DEVICE_MANUFACTURER
      + "\",\"model\":\"" + DEVICE_MODEL
      + "\",\"sw_version\":\"" + FIRMWARE_VERSION
      + "\",\"hw_version\":\"" + DEVICE_HW_VERSION + "\"}";

  String tempPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Temperature\","
    + "\"unique_id\":\"" + settings.deviceId + "_temperature\","
    + "\"device_class\":\"temperature\","
    + "\"unit_of_measurement\":\"°C\","
    + "\"state_class\":\"measurement\","
    + "\"suggested_display_precision\":2,"
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_TEMP + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_TEMP.c_str(), tempPayload.c_str(), true);

  String humPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Humidity\","
    + "\"unique_id\":\"" + settings.deviceId + "_humidity\","
    + "\"device_class\":\"humidity\","
    + "\"unit_of_measurement\":\"%\","
    + "\"state_class\":\"measurement\","
    + "\"suggested_display_precision\":1,"
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_HUMIDITY + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_HUMIDITY.c_str(), humPayload.c_str(), true);

  String battVPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Battery Voltage\","
    + "\"unique_id\":\"" + settings.deviceId + "_battery_voltage\","
    + "\"device_class\":\"voltage\","
    + "\"unit_of_measurement\":\"V\","
    + "\"state_class\":\"measurement\","
    + "\"suggested_display_precision\":2,"
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_BATTERY_V + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_BATTERY_V.c_str(), battVPayload.c_str(), true);

  String battPctPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Battery\","
    + "\"unique_id\":\"" + settings.deviceId + "_battery_percent\","
    + "\"device_class\":\"battery\","
    + "\"unit_of_measurement\":\"%\","
    + "\"state_class\":\"measurement\","
    + "\"suggested_display_precision\":0,"
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_BATTERY_PCT + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_BATTERY_PCT.c_str(), battPctPayload.c_str(), true);

  String rssiPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " WiFi Signal\","
    + "\"unique_id\":\"" + settings.deviceId + "_wifi_signal\","
    + "\"device_class\":\"signal_strength\","
    + "\"unit_of_measurement\":\"dBm\","
    + "\"state_class\":\"measurement\","
    + "\"entity_category\":\"diagnostic\","
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_RSSI + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_RSSI.c_str(), rssiPayload.c_str(), true);

  String lastUpdatePayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Last Update\","
    + "\"unique_id\":\"" + settings.deviceId + "_last_update\","
    + "\"device_class\":\"timestamp\","
    + "\"entity_category\":\"diagnostic\","
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_LAST_UPDATE + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_LAST_UPDATE.c_str(), lastUpdatePayload.c_str(), true);

  String resetReasonPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Reset Reason\","
    + "\"unique_id\":\"" + settings.deviceId + "_reset_reason\","
    + "\"entity_category\":\"diagnostic\","
    + "\"icon\":\"mdi:restart-alert\","
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_RESET_REASON + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_RESET_REASON.c_str(), resetReasonPayload.c_str(), true);

  String failCountPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Connect Fail Count\","
    + "\"unique_id\":\"" + settings.deviceId + "_connect_fail_count\","
    + "\"entity_category\":\"diagnostic\","
    + "\"state_class\":\"measurement\","
    + "\"icon\":\"mdi:wifi-alert\","
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_FAIL_COUNT + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_FAIL_COUNT.c_str(), failCountPayload.c_str(), true);

  String totalFailCountPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Total Fail Count\","
    + "\"unique_id\":\"" + settings.deviceId + "_total_fail_count\","
    + "\"entity_category\":\"diagnostic\","
    + "\"state_class\":\"total_increasing\","
    + "\"icon\":\"mdi:counter\","
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_TOTAL_FAIL_COUNT + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_TOTAL_FAIL_COUNT.c_str(), totalFailCountPayload.c_str(), true);

  String fwVersionPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Firmware Version\","
    + "\"unique_id\":\"" + settings.deviceId + "_firmware_version\","
    + "\"entity_category\":\"diagnostic\","
    + "\"icon\":\"mdi:chip\","
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_FW_VERSION + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_FW_VERSION.c_str(), fwVersionPayload.c_str(), true);

  String battLowPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Low Battery\","
    + "\"unique_id\":\"" + settings.deviceId + "_battery_low\","
    + "\"device_class\":\"battery\","
    + "\"entity_category\":\"diagnostic\","
    + "\"payload_on\":\"ON\","
    + "\"payload_off\":\"OFF\","
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_BATTERY_LOW + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_BATTERY_LOW.c_str(), battLowPayload.c_str(), true);

  // Last full charge date -- device_class "date" (not "timestamp": we only
  // ever record a calendar day, not a time-of-day). No expire_after: this
  // is a record of a past event, not a live reading, and should stay
  // visible even across a long gap between full charges.
  String lastFullChargePayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Last Full Charge\","
    + "\"unique_id\":\"" + settings.deviceId + "_last_full_charge\","
    + "\"device_class\":\"date\","
    + "\"entity_category\":\"diagnostic\","
    + "\"icon\":\"mdi:battery-charging-100\","
    + "\"state_topic\":\"" + TOPIC_LAST_FULL_CHARGE + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_LAST_FULL_CHARGE.c_str(), lastFullChargePayload.c_str(), true);

  String bootCountPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " Boot Count\","
    + "\"unique_id\":\"" + settings.deviceId + "_boot_count\","
    + "\"entity_category\":\"diagnostic\","
    + "\"state_class\":\"total_increasing\","
    + "\"icon\":\"mdi:counter\","
    + "\"expire_after\":" + String(EXPIRE_AFTER_SEC) + ","
    + "\"state_topic\":\"" + TOPIC_BOOT_COUNT + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_BOOT_COUNT.c_str(), bootCountPayload.c_str(), true);

  // Note: no expire_after here -- this is a control, not a reading, and
  // should stay usable in the UI even if the device has been quiet a while.
  // command_topic and state_topic are the same: the device echoes the
  // state back itself once it has actually acted on a request, so the
  // switch reflects reality (ON only briefly, until the next wake handles
  // it and reports back OFF) rather than just optimistically flipping.
  String otaRequestPayload = String("{")
    + "\"name\":\"" + settings.deviceName + " OTA Request\","
    + "\"unique_id\":\"" + settings.deviceId + "_ota_request\","
    + "\"entity_category\":\"config\","
    + "\"icon\":\"mdi:upload\","
    + "\"payload_on\":\"ON\","
    + "\"payload_off\":\"OFF\","
    + "\"optimistic\":false,"
    + "\"retain\":true,"
    + "\"command_topic\":\"" + TOPIC_OTA_REQUEST + "\","
    + "\"state_topic\":\"" + TOPIC_OTA_REQUEST + "\","
    + devBlock + "}";
  publishWithAck(DISCOVERY_OTA_REQUEST.c_str(), otaRequestPayload.c_str(), true);
}

int publishState(float tempC, float humidity, float battV, float battPct, int rssi, const String& resetReasonStr, uint32_t failCount) {
  char buf[16];

  int failed = 0;

  if (!isnan(tempC)) {
    dtostrf(tempC, 4, 2, buf);
    if (!publishWithAck(TOPIC_TEMP.c_str(), buf, true)) failed++;
  }
  if (!isnan(humidity)) {
    dtostrf(humidity, 4, 2, buf);
    if (!publishWithAck(TOPIC_HUMIDITY.c_str(), buf, true)) failed++;
  }

  dtostrf(battV, 4, 2, buf);
  if (!publishWithAck(TOPIC_BATTERY_V.c_str(), buf, true)) failed++;

  dtostrf(battPct, 4, 0, buf);
  if (!publishWithAck(TOPIC_BATTERY_PCT.c_str(), buf, true)) failed++;

  bool batteryLow = battPct < BATTERY_LOW_THRESHOLD_PCT;
  if (!publishWithAck(TOPIC_BATTERY_LOW.c_str(), batteryLow ? "ON" : "OFF", true)) failed++;

  String lastFullChargeDate = updateAndGetLastFullChargeDate(battPct);
  if (lastFullChargeDate.length() > 0) {
    if (!publishWithAck(TOPIC_LAST_FULL_CHARGE.c_str(), lastFullChargeDate.c_str(), true)) failed++;
  }

  snprintf(buf, sizeof(buf), "%d", rssi);
  if (!publishWithAck(TOPIC_RSSI.c_str(), buf, true)) failed++;

  if (!publishWithAck(TOPIC_RESET_REASON.c_str(), resetReasonStr.c_str(), true)) failed++;

  snprintf(buf, sizeof(buf), "%lu", (unsigned long)failCount);
  if (!publishWithAck(TOPIC_FAIL_COUNT.c_str(), buf, true)) failed++;

  snprintf(buf, sizeof(buf), "%lu", (unsigned long)totalFailCount);
  if (!publishWithAck(TOPIC_TOTAL_FAIL_COUNT.c_str(), buf, true)) failed++;

  if (!publishWithAck(TOPIC_FW_VERSION.c_str(), FIRMWARE_VERSION, true)) failed++;

  snprintf(buf, sizeof(buf), "%lu", (unsigned long)bootCount);
  if (!publishWithAck(TOPIC_BOOT_COUNT.c_str(), buf, true)) failed++;

  // NTP resync happens here, last -- after every other reading has already
  // published successfully. This way a slow or failed sync (a real network
  // round trip to an external server, unlike everything else which only
  // talks to the local broker) can only cost the last_update field, never
  // delay or risk the actual sensor data. Only attempted every N boots (or
  // if we've genuinely never synced) since 10-minute drift is negligible.
  bool dueForResync = !g_timeSynced || (bootCount % NTP_RESYNC_EVERY_N_BOOTS == 0);
  if (dueForResync) {
    syncTimeUtc();
  }

  // Read the timestamp last, right before publishing it, so it reflects
  // when this cycle actually finished rather than when it started.
  String lastUpdate = getCurrentTimestampUtc();
  if (lastUpdate.length() > 0) {
    if (!publishWithAck(TOPIC_LAST_UPDATE.c_str(), lastUpdate.c_str(), true)) failed++;
  }

  Serial.printf("Published: temp=%.2fC hum=%.2f%% battV=%.2f battPct=%.0f%% (low=%s) rssi=%ddBm lastUpdate=%s resetReason=%s failCount=%lu fw=%s (unacked topics: %d)\n",
                tempC, humidity, battV, battPct, batteryLow ? "yes" : "no", rssi, lastUpdate.c_str(), resetReasonStr.c_str(), (unsigned long)failCount, FIRMWARE_VERSION, failed);

  return failed;
}

// ---------------- Battery ----------------

// Detects a rising edge into 100% battery (i.e. "just charged", not "still
// sitting at 100% from before") and records today's UTC date as the new
// last-full-charge date. Persisted in flash/NVS rather than RTC memory
// specifically so it survives an actual battery depletion -- comparing
// this date to whenever the device later goes quiet is how you tell how
// long a charge actually lasted. Returns whatever date is currently
// stored (possibly still empty, if the battery has never yet read 100%
// since this was added).
//
// If the clock hasn't synced yet (g_timeSynced false) when a rising edge
// happens, wasAt100 still gets set so this doesn't re-trigger every wake,
// but no date gets recorded -- a one-time, cosmetic gap on a device's
// very first-ever boot.
String updateAndGetLastFullChargeDate(float batteryPercent) {
  batteryPrefs.begin("battery", false);
  bool wasAt100 = batteryPrefs.getBool("wasAt100", false);
  bool isAt100 = batteryPercent >= 100.0f;

  if (isAt100 && !wasAt100 && g_timeSynced) {
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    char buf[11];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
    batteryPrefs.putString("lastFullDate", buf);
    Serial.printf("[battery] reached 100%% -- recorded last full charge date: %s\n", buf);
  }
  if (isAt100 != wasAt100) {
    batteryPrefs.putBool("wasAt100", isAt100);
  }

  String result = batteryPrefs.getString("lastFullDate", "");
  batteryPrefs.end();
  return result;
}

float readBatteryVoltage() {
  // analogReadMilliVolts() uses the ESP32's factory ADC calibration (eFuse)
  // for an accurate mV reading -- far more accurate than manually mapping
  // raw analogRead() counts against an assumed 3.3V reference.
  // Averaged over BATT_ADC_SAMPLES reads to smooth out ADC noise.
  uint32_t sumMillivolts = 0;
  for (int i = 0; i < BATT_ADC_SAMPLES; i++) {
    sumMillivolts += analogReadMilliVolts(BATT_PIN);
    delay(2); // small gap between reads
  }
  uint32_t rawMillivolts = sumMillivolts / BATT_ADC_SAMPLES;
  float raw = (rawMillivolts / 1000.0f) * BATT_DIVIDER_RATIO;

  // Apply the same piecewise-linear correction as the ESPHome calibrate_linear filter
  if (raw <= BATT_CAL[0].raw) {
    // Extrapolate below the first point using the first segment's slope
    float slope = (BATT_CAL[1].actual - BATT_CAL[0].actual) / (BATT_CAL[1].raw - BATT_CAL[0].raw);
    return BATT_CAL[0].actual + (raw - BATT_CAL[0].raw) * slope;
  }
  if (raw >= BATT_CAL[BATT_CAL_POINTS - 1].raw) {
    // Extrapolate above the last point using the last segment's slope
    float slope = (BATT_CAL[BATT_CAL_POINTS - 1].actual - BATT_CAL[BATT_CAL_POINTS - 2].actual)
                 / (BATT_CAL[BATT_CAL_POINTS - 1].raw - BATT_CAL[BATT_CAL_POINTS - 2].raw);
    return BATT_CAL[BATT_CAL_POINTS - 1].actual + (raw - BATT_CAL[BATT_CAL_POINTS - 1].raw) * slope;
  }
  for (int i = 0; i < BATT_CAL_POINTS - 1; i++) {
    if (raw >= BATT_CAL[i].raw && raw <= BATT_CAL[i + 1].raw) {
      float slope = (BATT_CAL[i + 1].actual - BATT_CAL[i].actual) / (BATT_CAL[i + 1].raw - BATT_CAL[i].raw);
      return BATT_CAL[i].actual + (raw - BATT_CAL[i].raw) * slope;
    }
  }
  return raw; // unreachable, keeps the compiler happy
}

float batteryPercentage(float v) {
  float pct;

  // Same rescaled 4.15V-100%/3.20V-0% curve as the rest of this fleet's
  // battery-powered devices (proportionally compressed from the original
  // 4.20V-100%/3.20V-0% curve, not just flattened at the top) -- see
  // temp_humidity_sensor's own comment on why 4.20V is the wrong ceiling
  // for a resting, unplugged cell.
  if (v >= 4.15)      pct = 100.0;
  else if (v >= 4.10) pct = 95.0 + (v - 4.10) / 0.05 * 5.0;
  else if (v >= 4.06) pct = 90.0 + (v - 4.06) / 0.04 * 5.0;
  else if (v >= 4.01) pct = 85.0 + (v - 4.01) / 0.05 * 5.0;
  else if (v >= 3.96) pct = 80.0 + (v - 3.96) / 0.05 * 5.0;
  else if (v >= 3.91) pct = 75.0 + (v - 3.91) / 0.05 * 5.0;
  else if (v >= 3.87) pct = 70.0 + (v - 3.87) / 0.04 * 5.0;
  else if (v >= 3.80) pct = 65.0 + (v - 3.80) / 0.07 * 5.0;
  else if (v >= 3.72) pct = 60.0 + (v - 3.72) / 0.08 * 5.0;
  else if (v >= 3.68) pct = 55.0 + (v - 3.68) / 0.04 * 5.0;
  else if (v >= 3.66) pct = 50.0 + (v - 3.66) / 0.02 * 5.0;
  else if (v >= 3.62) pct = 45.0 + (v - 3.62) / 0.04 * 5.0;
  else if (v >= 3.58) pct = 40.0 + (v - 3.58) / 0.04 * 5.0;
  else if (v >= 3.54) pct = 35.0 + (v - 3.54) / 0.04 * 5.0;
  else if (v >= 3.49) pct = 30.0 + (v - 3.49) / 0.05 * 5.0;
  else if (v >= 3.44) pct = 25.0 + (v - 3.44) / 0.05 * 5.0;
  else if (v >= 3.39) pct = 20.0 + (v - 3.39) / 0.05 * 5.0;
  else if (v >= 3.34) pct = 15.0 + (v - 3.34) / 0.05 * 5.0;
  else if (v >= 3.30) pct = 10.0 + (v - 3.30) / 0.04 * 5.0;
  else if (v >= 3.25) pct =  5.0 + (v - 3.25) / 0.05 * 5.0;
  else if (v >= 3.20) pct =  0.0 + (v - 3.20) / 0.05 * 5.0;
  else                pct = 0.0;

  return roundf(pct);
}

// ---------------- Time ----------------

bool syncTimeUtc() {
  // The system clock survives deep sleep, so on every wake after the first,
  // it already holds a "valid-looking" value carried over from last cycle.
  // getLocalTime() only checks whether the year looks sane (>2016) -- it
  // doesn't confirm a fresh NTP packet actually arrived -- so without this,
  // it reports success instantly using the old, drifted value instead of
  // actually waiting for a real sync. Blanking the clock first forces a
  // genuine wait. We snapshot the old value first so a failed attempt can
  // restore it rather than leaving the clock blanked at epoch 0.
  struct timeval previous;
  gettimeofday(&previous, nullptr);

  struct timeval invalidate = {0, 0};
  settimeofday(&invalidate, nullptr);

  configTime(0, 0, NTP_SERVER); // 0, 0 = UTC, no DST offset

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, NTP_SYNC_TIMEOUT_MS)) {
    Serial.println("NTP sync failed this cycle.");
    if (g_timeSynced) {
      settimeofday(&previous, nullptr); // restore last known-good time rather than leaving it blanked
      Serial.println("Restored previous time (still using last successful sync).");
    }
    return false;
  }

  g_timeSynced = true;
  return true;
}

// Reads the current time from the already-synced system clock -- no network
// call. Call this right before actually publishing the timestamp, not
// earlier, so it reflects when the message really goes out rather than
// when NTP happened to sync at the start of the cycle (QoS 1 retries on
// earlier topics can otherwise leave a visible gap between the two).
String getCurrentTimestampUtc() {
  if (!g_timeSynced) return "";

  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);

  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

// Human-readable form of esp_reset_reason() — the key one to watch for is
// ESP_RST_BROWNOUT, which means the supply voltage sagged below the chip's
// threshold (usually a current spike, e.g. during WiFi TX) and it reset
// mid-cycle rather than completing a normal report.
String resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_EXT:       return "external_pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:  return "task_watchdog";
    case ESP_RST_WDT:       return "other_watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep_wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

// ---------------- LED / OTA ----------------

void blink(int times, uint32_t onMs, uint32_t gapMs) {
  uint32_t duty = ((uint32_t)LED_BRIGHTNESS_PCT * LED_PWM_MAX_DUTY) / 100;
  for (int i = 0; i < times; i++) {
    ledcWrite(LED_PIN, duty);
    delay(onMs);
    ledcWrite(LED_PIN, 0);
    if (i < times - 1) delay(gapMs);
  }
}

void runOtaWindow() {
  unsigned long start = millis();
  while (millis() - start < OTA_WINDOW_MS) {
    ArduinoOTA.handle();
    delay(10);
  }
  Serial.println("OTA window elapsed, resuming normal sleep cycle.");
}

// Triggered remotely via the "OTA Request" MQTT switch when the device is
// already configured and connected -- no portal, just a normal OTA window.
// Assumes WiFi is already connected.
void enterOtaMode() {
  Serial.println("Entering OTA mode, deep sleep prevented...");
  startAwakeWatchdog(OTA_WINDOW_MS + 30000); // OTA legitimately needs to stay awake this long
  ArduinoOTA.setHostname(settings.deviceId.c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
  ledcWrite(LED_PIN, ((uint32_t)LED_BRIGHTNESS_PCT * LED_PWM_MAX_DUTY) / 100); // solid LED = OTA mode active
  runOtaWindow();
  ledcWrite(LED_PIN, 0);
  blink(1, 50, 50); // brief off/on/off flourish before sleeping
}

// ---------------- Sleep ----------------

void goToSleep() {
  esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_US);

  // Wake immediately if the setup button is pressed, rather than waiting
  // for the next scheduled timer wake. The ESP32-C3 has no separate RTC IO
  // domain, so esp_deep_sleep_enable_gpio_wakeup works on any GPIO --
  // pressing the button pulls it LOW, which wakes the device straight into
  // setup(), where the existing digitalRead(SETUP_PIN) check already
  // detects it and enters maintenance mode. No separate reset needed.
  uint64_t setupPinMask = 1ULL << SETUP_PIN;
  esp_deep_sleep_enable_gpio_wakeup(setupPinMask, ESP_GPIO_WAKEUP_GPIO_LOW);

  Serial.println("Going to sleep...");
  Serial.flush();
  esp_deep_sleep_start();
}
