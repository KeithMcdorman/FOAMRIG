/*
 * Spray Foam Pressure Monitor + OTA
 *
 * Features:
 *  - Two high-pressure gauges (Iso / Resin, 0–1600 PSI)
 *  - Two air gauges (0–200 PSI)
 *  - Two low-side feed gauges (0–500 PSI)
 *  - Ratio indicator between Iso/Resin
 *  - Two relays: Spray/Park (25) and Drum Air (26)
 *  - Four DS18B20 temps:
 *      - Iso HP, Resin HP, Iso Low, Resin Low
 *  - Temp rings with per-gauge temp setpoints and shared min/max
 *  - WiFi:
 *      - Configurable AP or STA mode via Settings
 *      - AP SSID/PW + STA SSID/PW stored in NVS
 *  - Web OTA firmware update at /update
 *  - Firmware version string shown on main, settings, update pages
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <HardwareSerial.h>

#include <OneWire.h>
#include <DallasTemperature.h>
#include <ctype.h>
#include <math.h>

// ---------- Firmware version ----------
const char* FW_VERSION = "V2.0.0";

// ---------- Sensor pins ----------
// High-side pressures (0–1600 PSI)
#define SENSOR_A_PIN         35  // Iso (A side)
#define SENSOR_B_PIN         34  // Resin (B side)
// Air-side pressures (0–300 PSI)
#define SENSOR_AIRPISTON_PIN 32  // Primary air piston pressure
#define SENSOR_APAIR_PIN     33  // Fusion AP air purge pressure

// Low-side pressures (0–500 PSI) just before booster pumps
#define SENSOR_ISO_LOW_PIN    36  // Iso low pressure
#define SENSOR_RESIN_LOW_PIN  39  // Resin low pressure

// ---------- Relay outputs ----------
//
// 8-relay board mapping:
//
//   Relay 1 → GPIO32  (conflicts with SENSOR_AIRPISTON_PIN  – primary air)
//   Relay 2 → GPIO33  (conflicts with SENSOR_APAIR_PIN      – gun purge air)
//   Relay 3 → GPIO25  (used for Spray/Park relay)
//   Relay 4 → GPIO26  (used for Drum Pump Air relay)
//   Relay 5 → GPIO27  (now free – temp bus moved to GPIO18)
//   Relay 6 → GPIO14  (free)
//   Relay 7 → GPIO12  (free)
//   Relay 8 → GPIO13  (free)
//
// Keep the overlapping ones as commented #defines for reference so we
// don’t accidentally drive pins that are wired to pressure sensors.

// Board relays that conflict with analog pressure inputs:
// #define RELAY1_PIN 32   // Relay 1 – DO NOT USE (SENSOR_AIRPISTON_PIN)
// #define RELAY2_PIN 33   // Relay 2 – DO NOT USE (SENSOR_APAIR_PIN)

// Board relays we’re actually using today:
#define RELAY3_PIN 25      // Relay 3 – Spray/Park
#define RELAY4_PIN 26      // Relay 4 – Drum Pump Air

// Extra relays available for future use:
#define RELAY5_PIN 27      // Relay 5 – free (board label G27)
#define RELAY6_PIN 14      // Relay 6 – free (board label G14)
#define RELAY7_PIN 12      // Relay 7 – free (board label G12)
#define RELAY8_PIN 13      // Relay 8 – free (board label G13)

// Logical names the rest of the code already uses:
#define RELAY_SPRAY_PIN     RELAY3_PIN
#define RELAY_DRUM_AIR_PIN  RELAY4_PIN

// Hose heat relays (two sections)
#define RELAY_HOSE1_PIN     RELAY5_PIN
#define RELAY_HOSE2_PIN     RELAY6_PIN


// ---------- Hose-tip indicator LED ----------
// Mounted at the end of the spray hose.
//  - Solid: Iso HP within green band
//  - Slow blink: Iso HP below green band
//  - Fast blink: Iso HP above band (above setpoint area)
#define HOSE_LED_PIN 23
#define HOSE_LED_LEDC_CH   6
#define HOSE_LED_LEDC_FREQ 5000
#define HOSE_LED_LEDC_BITS 8
#define HOSE_LED_MAX_DUTY  ((1 << HOSE_LED_LEDC_BITS) - 1)
// Compatibility wrapper for Arduino-ESP32 LEDC API changes (v2 vs v3+)
#ifndef ESP_ARDUINO_VERSION_MAJOR
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static inline void hoseLedInit() {
  // New API: ledcAttach(pin, freq, resolution_bits)
  ledcAttach(HOSE_LED_PIN, HOSE_LED_LEDC_FREQ, HOSE_LED_LEDC_BITS);
  ledcWrite(HOSE_LED_PIN, 0);
}
static inline void hoseLedWrite(uint32_t duty) {
  ledcWrite(HOSE_LED_PIN, duty);
}
#else
static inline void hoseLedInit() {
  // Legacy API: ledcSetup(channel, freq, resolution_bits) + ledcAttachPin(pin, channel)
  ledcSetup(HOSE_LED_LEDC_CH, HOSE_LED_LEDC_FREQ, HOSE_LED_LEDC_BITS);
  ledcAttachPin(HOSE_LED_PIN, HOSE_LED_LEDC_CH);
  hoseLedWrite(0);
}
static inline void hoseLedWrite(uint32_t duty) {
  ledcWrite(HOSE_LED_LEDC_CH, duty);
}
#endif


// ---------- DS18B20 temperature support ----------
// NOTE: bus moved from 27 to 18 to free 27 for relays/IO
#define ONE_WIRE_BUS_PIN 18   // All four sensors on this pin

OneWire oneWire(ONE_WIRE_BUS_PIN);
DallasTemperature tempSensors(&oneWire);

// DS18B20 device count and temps (°F)
int   dsDeviceCount = 0;
float isoTempF        = NAN;  // Iso HP outlet temp
float resinTempF      = NAN;  // Resin HP outlet temp
float isoLowTempF     = NAN;  // Iso low-side feed temp
float resinLowTempF   = NAN;  // Resin low-side feed temp

// Assignment of sensors by ROM address
DeviceAddress isoTempAddr;
DeviceAddress resinTempAddr;
DeviceAddress isoLowTempAddr;
DeviceAddress resinLowTempAddr;

bool isoTempAssigned      = false;
bool resinTempAssigned    = false;
bool isoLowTempAssigned   = false;
bool resinLowTempAssigned = false;

// Stored as hex strings in NVS (e.g. "28FF3C91A21604B2")
String isoTempAddrStr;
String resinTempAddrStr;
String isoLowTempAddrStr;
String resinLowTempAddrStr;

// ---------- WiFi configuration ----------
enum NetworkModeType { NETMODE_AP = 0, NETMODE_STA = 1 };

const char* DEFAULT_AP_SSID = "Foam";
const char* DEFAULT_AP_PASS = "1234567890";

int    networkMode = NETMODE_AP;
String apSsid      = DEFAULT_AP_SSID;
String apPass      = DEFAULT_AP_PASS;
String staSsid     = "";
String staPass     = "";

// WiFi fallback tracking
unsigned long wifiDisconnSince   = 0;  // millis when we first noticed disconnect
bool          apFallbackActive   = false;
const unsigned long WIFI_FALLBACK_MS = 5UL * 60UL * 1000UL; // 5 minutes

// Web stack
WebServer        server(80);
WebSocketsServer webSocket(81);
Preferences      prefs;

// UART link to HMI (ESP32-P4 display)
HardwareSerial HMISerial(2);
static String hmiRxBuffer;

// Last status JSON for HTTP fallback (/api/live)
static String lastStatusJson;



// ---------- JSON helpers (ArduinoJson) ----------
// Keep telemetry JSON creation robust and future-proof as fields expand.
// Avoid ad-hoc String concatenation to prevent subtle formatting bugs.
static inline float round1(float v) {
  if (isnan(v)) return v;
  return roundf(v * 10.0f) / 10.0f;
}

// Capacity for the live status payload (pressures, temps, relay states, config, interlock).
// If you add lots of new fields later (e.g., hose heat zones), bump this.
// JSON_OBJECT_SIZE(32) is 704 bytes; we add headroom for strings and growth.
static constexpr size_t STATUS_JSON_DOC_CAP = 2048;
static constexpr size_t STATUS_JSON_OUT_MAX = 1024;
static char statusJsonBuf[STATUS_JSON_OUT_MAX];



// Global settings (loaded from Preferences)
int  targetPressure = 1000;
int  marginPercent  = 10;
int  diffPressure   = 50;

// Low pressure set points for air gauges (defaults to 100 PSI each).
int  airTarget   = 100;
int  gunTarget   = 100;

// Low-side Iso/Resin setpoints (0–500 PSI feed just before booster)
int  isoLowTarget   = 200;
int  resinLowTarget = 200;

// Supply low interlock threshold (PSI) for low-side pressures.
int  supplyLowPSI   = 150;

// Temperature gauge configuration
int  isoTempTargetF      = 120; // Iso HP outlet target temp (°F)
int  resinTempTargetF    = 120; // Resin HP outlet target temp (°F)
int  isoLowTempTargetF   = 120; // Iso low-side target temp (°F)
int  resinLowTempTargetF = 120; // Resin low-side target temp (°F)
int  tempMinF            = 40;  // Temp ring lower bound (°F)
int  tempMaxF            = 180; // Temp ring upper bound (°F)

// --- calibration constants (loaded from prefs) ---
// raw reading at 0 PSI per sensor
float isoR0   = 0.0f;
float resinR0 = 0.0f;
float airR0   = 0.0f;
float apAirR0 = 0.0f;
// scale factors K = P_cal / (R1 – R0)
float isoK    = 1.0f;
float resinK  = 1.0f;
float airK    = 1.0f;
float apAirK  = 1.0f;

// Low-side Iso/Resin calibration
float isoLowR0   = 0.0f;
float isoLowK    = 1.0f;
float resinLowR0 = 0.0f;
float resinLowK  = 1.0f;

// ---------- Relay state & low-side tracking ----------
bool drumAirEnabled = false;  // relay on 26
bool sprayEnabled   = false;  // relay on 25

float lastIsoLowPSI   = 0.0f;
float lastResinLowPSI = 0.0f;
float lastIsoHPPSI    = 0.0f;
float lastResinHPPSI  = 0.0f;

// ---------- Hose heat control (2 zones) ----------
// Backend-only for now: exposes enable/setpoint/tolerance + relay output state
// via status JSON and /api endpoints so the HMI can fully emulate the web UI later.

bool hose1Enabled = false;
bool hose2Enabled = false;

// Setpoints and tolerance (°F). Tolerance is a +/- hysteresis band to minimize cycling.
int hose1SetF = 125;
int hose2SetF = 125;
int hose1TolF = 3;
int hose2TolF = 3;
int hoseOvertempF = 5;   // °F above setpoint allowed before safety trip
bool hoseOvertempActive = false;

// Current relay call-for-heat state (latched by hysteresis logic)
bool hose1Heating = false;
bool hose2Heating = false;

// Hose temperature readings (°F) from DS18B20 assignments
float hose1TempF = NAN;
float hose2TempF = NAN;

// DS18B20 assignments for hose heat sensors
DeviceAddress hose1TempAddr;
DeviceAddress hose2TempAddr;
bool hose1TempAssigned = false;
bool hose2TempAssigned = false;
String hose1TempAddrStr;
String hose2TempAddrStr;

// Hose-end status LED mode
enum HoseLedMode {
  HOSE_LED_OFF        = 0,
  HOSE_LED_SOLID      = 1,
  HOSE_LED_BLINK_SLOW = 2,
  HOSE_LED_BLINK_FAST = 3,
  HOSE_LED_PULSE      = 4
};

HoseLedMode hoseLedMode = HOSE_LED_PULSE;

// ---------- Interlock state ----------
bool   sprayInterlockActive = false;
String lastInterlockReason;

// Build a more actionable interlock banner by snapshotting pressures at the moment
// the interlock is latched (so values don't drift while troubleshooting).
static inline String fmtPsi1(float v) {
  if (isnan(v)) return String("NA");
  return String(v, 1);
}

static inline String makeLowSupplyInterlockReason(float isoHP, float resinHP,
                                                  float isoLow, float resinLow,
                                                  int minPsi)
{
  // Keep the canonical phrase "low supply pressure on feed side" so existing
  // reset-condition checks continue to match.
  String s = "Interlock: low supply pressure on feed side.";
  s += " HP Iso " + fmtPsi1(isoHP) + " PSI, Resin " + fmtPsi1(resinHP) + " PSI";
  s += " | Feed IsoLow " + fmtPsi1(isoLow) + " PSI, ResinLow " + fmtPsi1(resinLow) + " PSI";
  s += " (min " + String(minPsi) + " PSI).";
  return s;
}

// ---------- Helpers for DS18B20 addresses ----------

String addressToString(const uint8_t addr[8]) {
  char buf[17];
  for (int i = 0; i < 8; i++) {
    sprintf(&buf[i * 2], "%02X", addr[i]);
  }
  buf[16] = '\0';
  return String(buf);
}

bool parseAddressString(const String& s, uint8_t addr[8]) {
  if (s.length() != 16) return false;
  auto hexVal = [](char c) -> int {
    c = toupper((unsigned char)c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (int i = 0; i < 8; i++) {
    char high = s[2 * i];
    char low  = s[2 * i + 1];
    int hi = hexVal(high);
    int lo = hexVal(low);
    if (hi < 0 || lo < 0) return false;
    addr[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// Just for debugging: show discovered sensors & which are assigned
void printDS18B20Addresses() {
  Serial.println("=== Temp assignments restored from NVS ===");

  Serial.printf("Iso HP:      '%s'\n", isoTempAddrStr.c_str());
  Serial.printf("Resin HP:    '%s'\n", resinTempAddrStr.c_str());
  Serial.printf("Iso Low:     '%s'\n", isoLowTempAddrStr.c_str());
  Serial.printf("Resin Low:   '%s'\n", resinLowTempAddrStr.c_str());
  Serial.printf("Hose 1:      '%s'\n", hose1TempAddrStr.c_str());
  Serial.printf("Hose 2:      '%s'\n", hose2TempAddrStr.c_str());

  Serial.printf(
      "Assigned flags: iso=%d resin=%d isoLow=%d resinLow=%d hose1=%d hose2=%d\n",
      (int)isoTempAssigned, (int)resinTempAssigned,
      (int)isoLowTempAssigned, (int)resinLowTempAssigned,
      (int)hose1TempAssigned, (int)hose2TempAssigned);

  dsDeviceCount = tempSensors.getDeviceCount();
  Serial.printf("Found %d DS18B20 device(s) on bus\n", dsDeviceCount);

  for (int i = 0; i < dsDeviceCount; i++) {
    DeviceAddress addr;
    if (tempSensors.getAddress(addr, i)) {
      String s = addressToString(addr);
      Serial.printf("  Index %d address: %s\n", i, s.c_str());
    } else {
      Serial.printf("  Index %d: <no address>\n", i);
    }
  }

  // Reprint assignments in a compact, aligned form (useful after /api/temp-sensors updates)
  Serial.printf("Assigned Iso HP Temp:    %s\n", isoTempAddrStr.c_str());
  Serial.printf("Assigned Resin HP Temp:  %s\n", resinTempAddrStr.c_str());
  Serial.printf("Assigned Iso Low Temp:   %s\n", isoLowTempAddrStr.c_str());
  Serial.printf("Assigned Resin Low Temp: %s\n", resinLowTempAddrStr.c_str());
  Serial.printf("Assigned Hose 1 Temp:    %s\n", hose1TempAddrStr.c_str());
  Serial.printf("Assigned Hose 2 Temp:    %s\n", hose2TempAddrStr.c_str());
}

// ---------- WiFi / network ----------

void setupWiFi() {
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);

  // Reset fallback tracking on fresh WiFi setup
  wifiDisconnSince = 0;
  apFallbackActive = false;

  if (networkMode == NETMODE_STA && staSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    Serial.printf("Connecting to STA SSID: %s\n", staSsid.c_str());

    // Try briefly at boot; if not connected we'll rely on the
    // non-blocking fallback timer in handleWifiFallback().
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("STA connected, IP: %s\n", WiFi.localIP().toString().c_str());
      return;
    } else {
      Serial.println("STA not yet connected; will fall back to AP if disconnected for >5 minutes.");
      // Start disconnect timer; handleWifiFallback() will watch this.
      wifiDisconnSince = millis();
      return;
    }
  }

  // AP mode (either explicitly selected or no STA SSID configured)
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(apSsid.c_str(), apPass.c_str());
  delay(200);
  Serial.printf("AP start (%s): %s  IP: %s\n",
                apSsid.c_str(),
                apOk ? "OK" : "FAILED",
                WiFi.softAPIP().toString().c_str());
}

// Non-blocking WiFi watchdog: if STA drops and stays disconnected
// for more than WIFI_FALLBACK_MS, start the AP as a fallback.
void handleWifiFallback() {
  // Only meaningful when STA is the configured mode
  if (networkMode != NETMODE_STA) {
    wifiDisconnSince = 0;
    apFallbackActive = false;
    return;
  }

  if (apFallbackActive) {
    // Already switched to AP fallback; nothing else to do here.
    return;
  }

  wl_status_t st = WiFi.status();
  unsigned long now = millis();

  if (st == WL_CONNECTED) {
    // Connected: clear any previous disconnect timer.
    wifiDisconnSince = 0;
    return;
  }

  // Disconnected
  if (wifiDisconnSince == 0) {
    // First time we noticed the disconnect; start the timer.
    wifiDisconnSince = now;
    return;
  }

  if (now - wifiDisconnSince < WIFI_FALLBACK_MS) {
    // Not yet past the timeout.
    return;
  }

  // We've been disconnected long enough; bring up AP as a fallback.
  Serial.println("WiFi disconnected for >5 minutes, starting AP fallback.");

  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(apSsid.c_str(), apPass.c_str());
  delay(200);

  Serial.printf("AP fallback start (%s): %s  IP: %s\n",
                apSsid.c_str(),
                apOk ? "OK" : "FAILED",
                WiFi.softAPIP().toString().c_str());

  apFallbackActive = true;

  // Re-start mDNS in AP mode so foam.local still works.
  MDNS.end();
  if (MDNS.begin("foam")) {
    Serial.println("mDNS: foam.local (AP fallback)");
  } else {
    Serial.println("mDNS failed in AP fallback");
  }
}

// ---------- OTA update page ----------

const char* updatePage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>Foam Rig Firmware Update</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <meta name="apple-mobile-web-app-capable" content="yes" />
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent" />
  <style>
    :root {
      --bg: #020617;
      --panel: #020617;
      --panel-border: #1e293b;
      --accent: #38bdf8;
      --accent-soft: rgba(56,189,248,0.1);
      --accent-soft-border: rgba(56,189,248,0.35);
      --text-main: #e5e7eb;
      --text-muted: #9ca3af;
      --good: #22c55e;
      --bad: #ef4444;
      --gap: 0.4rem;
    }

    * {
      box-sizing: border-box;
      -webkit-font-smoothing: antialiased;
    }

    body {
      margin: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont,
        "SF Pro Text", "Segoe UI", sans-serif;
      background: radial-gradient(circle at top, #1e293b 0, #020617 55%, #000 100%);
      color: var(--text-main);
      display: flex;
      justify-content: center;
      align-items: flex-start;
      min-height: 100vh;
      padding: 1.25rem;
    }

    .app {
      width: 100%;
      max-width: 480px;
      background: linear-gradient(145deg, rgba(15,23,42,0.95), rgba(15,23,42,0.9));
      border-radius: 1.25rem;
      border: 1px solid rgba(148,163,184,0.18);
      box-shadow:
        0 22px 55px rgba(15,23,42,0.9),
        0 0 0 1px rgba(15,23,42,0.6);
      padding: 1.4rem 1.5rem 1.7rem;
    }

    h1 {
      margin: 0 0 0.3rem;
      font-size: 1.25rem;
      letter-spacing: 0.02em;
    }

    .subtitle {
      font-size: 0.85rem;
      color: var(--text-muted);
      margin-bottom: 1rem;
    }

    .card {
      border-radius: 1rem;
      border: 1px solid rgba(30,64,175,0.7);
      background:
        radial-gradient(circle at 0 0, rgba(56,189,248,0.12), transparent 55%),
        radial-gradient(circle at 100% 0, rgba(59,130,246,0.16), transparent 55%),
        #020617;
      padding: 1rem 1rem 0.9rem;
    }

    .field {
      display: flex;
      flex-direction: column;
      gap: 0.35rem;
      margin-bottom: 0.7rem;
      font-size: 0.85rem;
    }

    label {
      color: var(--text-muted);
    }

    input[type="file"] {
      font-size: 0.85rem;
      color: var(--text-main);
    }

    .btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      border-radius: 999px;
      border: none;
      padding: 0.55rem 1.35rem;
      background: linear-gradient(135deg,#38bdf8,#0ea5e9);
      color: #020617;
      font-size: 0.9rem;
      font-weight: 600;
      cursor: pointer;
      box-shadow:
        0 0 0 1px rgba(15,23,42,0.85),
        0 14px 30px rgba(56,189,248,0.45);
      transition: transform 0.1s ease, box-shadow 0.1s ease, opacity 0.1s ease;
    }

    .btn:active {
      transform: translateY(1px) scale(0.99);
      box-shadow:
        0 0 0 1px rgba(15,23,42,0.85),
        0 8px 18px rgba(56,189,248,0.35);
    }

    .btn:disabled {
      opacity: 0.55;
      cursor: default;
      box-shadow:
        0 0 0 1px rgba(15,23,42,0.85),
        0 6px 14px rgba(15,23,42,0.85);
    }

    .upload-row {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 0.6rem;
      margin-top: 0.4rem;
    }

    .upload-status {
      font-size: 0.8rem;
      color: var(--text-muted);
    }

    .version {
      font-size: 0.78rem;
      color: var(--text-muted);
      margin-top: 0.7rem;
    }

    .nav-row {
      display: flex;
      justify-content: space-between;
      margin-top: 1rem;
      font-size: 0.8rem;
    }

    a.nav {
      color: var(--accent);
      text-decoration: none;
    }
  </style>
</head>
<body>
<div class="app">
  <h1>Firmware Update</h1>
  <div class="subtitle">
    Current firmware: <strong>{{FW_VERSION}}</strong><br/>
    Upload a compiled <code>firmware.bin</code> to update the rig.
  </div>

  <div class="card">
    <form id="otaForm" method="POST" action="/update" enctype="multipart/form-data">
      <div class="field">
        <label for="firmware">Firmware binary (.bin)</label>
        <!-- Limit picker to .bin files; browser still remembers last folder -->
        <input type="file" id="firmware" name="firmware" accept=".bin" required />
      </div>
      <div class="upload-row">
        <button class="btn" type="submit" id="uploadBtn">Upload &amp; Flash</button>
        <div id="uploadStatus" class="upload-status"></div>
      </div>
    </form>
    <div class="version">
      After a successful update the rig will reboot automatically.
    </div>
  </div>

  <div class="nav-row">
    <a class="nav" href="/settings">⬅ Settings</a>
    <a class="nav" href="/">Live Dashboard ➜</a>
  </div>
</div>

<script>
  document.addEventListener('DOMContentLoaded', function() {
    var form   = document.getElementById('otaForm');
    var btn    = document.getElementById('uploadBtn');
    var status = document.getElementById('uploadStatus');

    if (!form) return;

    form.addEventListener('submit', function() {
      if (btn)    btn.disabled = true;
      if (status) status.textContent = 'Uploading Please wait....';
    });
  });
</script>
</body>
</html>
)rawliteral";

// ---------- MAIN PAGE (live gauges) ----------

const char* mainPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>Spray Foam Pressure Monitor</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <!-- iOS fullscreen when “Add to Home Screen” -->
  <meta name="apple-mobile-web-app-capable" content="yes" />
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent" />
  <style>
    :root {
      --bg: #0f172a;
      --card-bg: #020617;
      --card-border: #1e293b;
      --accent: #38bdf8;
      --text-main: #e5e7eb;
      --text-muted: #9ca3af;
      --good: #22c55e;
      --bad: #ef4444;
    }

    * {
      box-sizing: border-box;
      -webkit-font-smoothing: antialiased;
    }

    body {
      margin: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont,
        "SF Pro Text", "Segoe UI", sans-serif;
      background: radial-gradient(circle at top, #1e293b 0, #020617 55%, #000 100%);
      color: var(--text-main);
      height: 100vh;
      overflow: hidden; /* ensure the dashboard fits on screen */
    }

    .app {
      height: 100vh;
      padding: 0.4rem 0.8rem 0.6rem;
      display: flex;
      flex-direction: column;
    }

    header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 0.25rem;
    }

    .title-group {
      display: flex;
      flex-direction: column;
    }

    h1 {
      margin: 0;
      font-size: 1.25rem;
      letter-spacing: 0.04em;
      text-transform: uppercase;
    }

    .subtitle {
      font-size: 0.75rem;
      color: var(--text-muted);
      margin-top: 0.1rem;
    }

    .status-pill {
      display: inline-flex;
      align-items: center;
      gap: 0.35rem;
      font-size: 0.7rem;
      padding: 0.2rem 0.55rem;
      border-radius: 999px;
      background: rgba(15, 23, 42, 0.85);
      border: 1px solid rgba(148, 163, 184, 0.35);
    }

    .status-dot {
      width: 0.45rem;
      height: 0.45rem;
      border-radius: 999px;
      background: var(--bad);
      box-shadow: 0 0 8px rgba(239, 68, 68, 0.8);
    }

    .settings-btn {
      border-radius: 999px;
      padding: 0.3rem 0.8rem;
      background: rgba(15, 23, 42, 0.9);
      border: 1px solid rgba(148, 163, 184, 0.5);
      color: var(--text-main);
      font-size: 0.8rem;
      display: inline-flex;
      align-items: center;
      gap: 0.3rem;
      text-decoration: none;
    }

    .settings-btn span.icon {
      font-size: 0.9rem;
    }

    
    .reset-btn {
      border-radius: 999px;
      padding: 0.3rem 0.8rem;
      background: rgba(239, 68, 68, 0.14);
      border: 1px solid rgba(239, 68, 68, 0.55);
      color: var(--text-main);
      font-size: 0.8rem;
      display: inline-flex;
      align-items: center;
      gap: 0.3rem;
      cursor: pointer;
      user-select: none;
    }
    .reset-btn:hover { filter: brightness(1.08); }
    .reset-btn:disabled { opacity: 0.55; cursor: not-allowed; }

    .is-hidden { display: none !important; }
.settings-btn:active {
      transform: translateY(1px);
    }

    /* Global alert/status bar above the gauges */
    .alert-bar {
      min-height: 1.1rem;
      font-size: 0.75rem;
      color: #e5e7eb;
      text-align: center;
      padding: 0.15rem 0.4rem;
      margin-bottom: 0.25rem;
      border-radius: 0.5rem;
      border: 1px solid transparent;
    }
    .alert-active {
      background: rgba(127, 29, 29, 0.35);
      border-color: #b91c1c;
      color: #fca5a5;
      box-shadow: 0 0 10px rgba(248, 113, 113, 0.45);
    }

    /* Layout for gauges */
    .dashboard {
      flex: 1;
      display: flex;
      flex-direction: column;
      gap: 0.4rem;
    }

    .top-row {
  display: grid;
  grid-template-columns: minmax(260px, 1fr) minmax(260px, 0.95fr) minmax(260px, 1fr);
  gap: var(--gap);
  align-items: stretch;
}

.bottom-row {
  display: grid;
  grid-template-columns: repeat(6, minmax(0, 1fr));
  gap: var(--gap);
  align-items: stretch;
}
.gauge-card {
      background: radial-gradient(circle at top left, #0b1120 0, #020617 60%);
      border-radius: 16px;
      border: 1px solid var(--card-border);
      padding: 0.35rem 0.35rem 0.45rem;
      display: flex;
      flex-direction: column;
      align-items: center;
      box-shadow: 0 14px 30px rgba(15, 23, 42, 0.9);
    }

    .gauge-card.large {
  width: 100%;
  max-width: none;
  min-width: 0;
}

    .gauge-card.small {
      width: 20vw;
      max-width: 220px;
      min-width: 160px;
    }

    .placeholder { visibility: hidden; }

    .gauge-header {
      font-size: 0.75rem;
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--text-muted);
      margin-bottom: 0.15rem;
    }

    .gauge-value {
      font-size: 0.85rem;
      margin-top: 0.15rem;
      color: var(--text-main);
    }

    .gauge-value.secondary {
      font-size: 0.75rem;
      color: var(--text-muted);
      margin-top: 0.05rem;
    }

    canvas.gauge {
      width: 100%;
      height: auto;
      display: block;
    }

    /* Center column: Spray (top), Ratio (middle), Drum Air (bottom) */
    .center-column {
      display: flex;
      flex-direction: column;
      gap: var(--gap);
      align-items: stretch;
      height: 100%;
      width: 100%;
      max-width: none;
      min-width: 0;
    }

    /* Make the center stack fill the same vertical space as the big gauges */
    .center-column .action-row,
    .center-column .enable-row {
      flex: 1.2 1 0;
      align-items: stretch;
    }

    .center-column .status-row {
      flex: 1 1 0;
      align-items: stretch;
    }

    .center-column .action-row .mode-btn {
      height: 100%;
    }

    

    .center-column .enable-row .mode-btn {
      height: 100%;
      /* Stack label over status (Hose enable buttons) */
      flex-direction: column;
      gap: 0.18rem;
    }

.center-column .status-row .hose-status-btn,
    .center-column .status-row .ratio-card {
      height: 100%;
      min-height: 0;
    }

    .btn-row {
      display: flex;
      gap: var(--gap);
      width: 100%;
    }

    .triple-row {
      display: flex;
      gap: var(--gap);
      width: 100%;
      align-items: stretch;
    }

    .triple-row > * {
      flex: 1;
    }

    .btn-label {
      display: block;
      font-size: 0.65rem;
      letter-spacing: 0.12em;
      opacity: 0.82;
      margin-bottom: 0.12rem;
    }

    .btn-sub {
      display: block;
      font-size: 1.05rem;
      font-weight: 700;
      letter-spacing: 0.06em;
      opacity: 1;
      margin-top: 0.05rem;
    }

    .hose-status-btn {
      border-radius: 16px;
      border: 1px solid rgba(148, 163, 184, 0.6);
      background: rgba(15, 23, 42, 0.98);
      color: var(--text-main);
      padding: 0.35rem 0.35rem;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      text-align: center;
          }

    .hose-status-btn.heating {
      background: linear-gradient(135deg, #22c55e, #16a34a);
      color: #020617;
      border-color: rgba(22,163,74,0.9);
      box-shadow: 0 0 10px rgba(22,163,74,0.7);
    }

    /* Enabled, not actively heating ("At Temp") */
    .hose-status-btn.attemp {
      background: linear-gradient(135deg, #0ea5e9, #2563eb);
      color: #020617;
      border-color: rgba(37,99,235,0.9);
      box-shadow: 0 0 10px rgba(37,99,235,0.55);
    }

    .hose-label {
      font-size: 0.75rem;
      letter-spacing: 0.12em;
      text-transform: uppercase;
      color: var(--text-muted);
      margin-bottom: 0.25rem;
    }

    .hose-temp {
      font-size: 1.05rem;
      font-weight: 700;
      line-height: 1.2;
    }

    .hose-state {
      margin-top: 0.15rem;
      font-size: 0.7rem;
      letter-spacing: 0.1em;
      text-transform: uppercase;
      opacity: 0.9;
    }

    .bottom-row .gauge-card.small {
      width: 100%;
      max-width: none;
      min-width: 0;
    }

    .setpoint-card {
      padding: 0;
      overflow: hidden;
      justify-content: stretch;
    }

    .setpoint-card .sp-btn {
      width: 100%;
      flex: 0 0 25%;
      border: none;
      background: rgba(15, 23, 42, 0.98);
      color: var(--text-main);
      font-size: 1.25rem;
      font-weight: 800;
      padding: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      border-bottom: 1px solid rgba(148, 163, 184, 0.35);
    }

    .setpoint-card .sp-btn.sp-down {
      border-top: 1px solid rgba(148, 163, 184, 0.35);
      border-bottom: none;
    }

    .setpoint-card .sp-center {
      flex: 0 0 50%;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      padding: 0.35rem 0.25rem;
      gap: 0.2rem;
    }

    .setpoint-card .sp-label {
      font-size: 0.75rem;
      letter-spacing: 0.12em;
      text-transform: uppercase;
      color: var(--text-muted);
      text-align: center;
    }

    .setpoint-card .sp-value {
      font-size: 1.2rem;
      font-weight: 800;
    }


    .ratio-card {
      padding: 0.3rem 0.3rem 0.45rem;
      background: radial-gradient(circle at top, #020617 0, #020617 55%);
      border-radius: 16px;
      border: 1px solid var(--card-border);
      box-shadow: 0 14px 30px rgba(15, 23, 42, 0.9);
      display: flex;
      flex-direction: column;
      align-items: center;
      flex: 1;
    }

    .ratio-label-top {
      font-size: 0.75rem;
      letter-spacing: 0.12em;
      text-transform: uppercase;
      color: var(--text-muted);
      margin-bottom: 0.25rem;
    }

    .ratio-circle {
      width: 100%;
      border-radius: 50%;
      border: 2px solid rgba(148, 163, 184, 0.4);
      background: radial-gradient(circle at 30% 0%, rgba(148, 163, 184, 0.15), transparent 55%);
      position: relative;
      padding-top: 100%;
      transition: background-color 0.2s ease, box-shadow 0.2s ease, border-color 0.2s ease;
    }

    .ratio-inner {
      position: absolute;
      inset: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      text-align: center;
    }

    .ratio-text {
      font-size: 1.0rem;
      font-weight: 600;
      letter-spacing: 0.3em;
    }

    .ratio-good {
      background-color: rgba(34, 197, 94, 0.16);
      border-color: rgba(34, 197, 94, 0.9);
      box-shadow: 0 0 14px rgba(34, 197, 94, 0.9);
    }

    .ratio-bad {
      background-color: rgba(239, 68, 68, 0.16);
      border-color: rgba(239, 68, 68, 0.9);
      box-shadow: 0 0 14px rgba(239, 68, 68, 0.9);
    }

    .mode-btn {
      width: 100%;
      padding: 0.35rem 0.4rem;
      border-radius: 999px;
      border: 1px solid rgba(148, 163, 184, 0.6);
      background: rgba(15, 23, 42, 0.98);
      color: var(--text-main);
      font-size: 0.8rem;
      text-transform: uppercase;
      letter-spacing: 0.1em;
      flex: 1;
      display: flex;
      align-items: center;
      justify-content: center;
      text-align: center;
    }

    .mode-btn.on {
      background: linear-gradient(135deg, #22c55e, #16a34a);
      color: #020617;
      border-color: rgba(22,163,74,0.9);
      box-shadow: 0 0 10px rgba(22,163,74,0.7);
    }

    .mode-btn.interlock {
      background: linear-gradient(135deg, #ef4444, #b91c1c);
      color: #fee2e2;
      border-color: rgba(248,113,113,0.9);
      box-shadow: 0 0 10px rgba(248,113,113,0.75);
    }

    footer {
      margin-top: 0.15rem;
      font-size: 0.65rem;
      color: var(--text-muted);
      display: flex;
      justify-content: space-between;
      align-items: center;
    }

    .footer-right {
      opacity: 0.7;
    }

    @media (max-width: 900px) {
      body {
        overflow-y: auto;
      }
      .app {
        height: auto;
        min-height: 100vh;
      }
      .dashboard {
        gap: 0.6rem;
      }
      .top-row,
      .bottom-row {
        grid-template-columns: 1fr;
        gap: 0.6rem;
      }
      .gauge-card.large,
      .gauge-card.small {
        width: 100%;
        max-width: none;
        min-width: 0;
      }
      .center-column {
        width: 100%;
        max-width: none;
        min-width: 0;
        height: auto;
      }
      .center-column .action-row,
      .center-column .status-row,
      .center-column .enable-row {
        flex: 0 0 auto;
      }
      .ratio-card {
        margin: 0;
      }
    }
  </style>
</head>
<body>
<div class="app">
  <header>
    <div class="title-group">
      <h1>Spray Foam Pressure Monitor</h1>
      <div class="subtitle">foam.local • High-pressure &amp; Air System</div>
    </div>
    <div style="display:flex;align-items:center;gap:0.6rem;">
      <div class="status-pill">
        <div id="liveDot" class="status-dot"></div>
        <span id="liveStatusText">Connecting…</span>
      </div>
      <button id="interlockResetBtn" type="button" class="reset-btn is-hidden">Reset</button>
      <a href="/settings" class="settings-btn">
        <span class="icon">⚙️</span>
        <span>Settings</span>
      </a>
    </div>
  </header>

  <!-- Global interlock / status message bar -->
  <div id="alertBar" class="alert-bar"></div>

  <main class="dashboard">
    <!-- Top row: big high-pressure gauges with center control column -->
    <div class="top-row">
      <div class="gauge-card large left">
        <div class="gauge-header">Iso Pressure</div>
        <canvas id="isoGauge" class="gauge"></canvas>
        <div class="gauge-value" id="isoValue">0 PSI</div>
        <div class="gauge-value secondary" id="isoTempValue">-- °F</div>
      </div>

      <div class="center-column">
        <div class="btn-row action-row">
          <button id="sprayBtn" class="mode-btn">Spray</button>
          <button id="drumAirBtn" class="mode-btn">Drum Air</button>
        </div>

        <div class="triple-row status-row">
          <button id="hose1StatusBtn" class="hose-status-btn">
            <div class="hose-label">Hose 1</div>
            <div class="hose-temp" id="hose1TempDisplay">-- °F</div>
            <div class="hose-state" id="hose1StateDisplay">OFF</div>
          </button>

          <div class="ratio-card">
          <div class="ratio-label-top">Ratio</div>
          <div id="ratioCircle" class="ratio-circle">
            <div class="ratio-inner">
              <div id="ratioText" class="ratio-text">RATIO</div>
            </div>
          </div>
        </div>

          <button id="hose2StatusBtn" class="hose-status-btn">
            <div class="hose-label">Hose 2</div>
            <div class="hose-temp" id="hose2TempDisplay">-- °F</div>
            <div class="hose-state" id="hose2StateDisplay">OFF</div>
          </button>
        </div>

        <div class="btn-row enable-row">
          <button id="hose1EnableBtn" class="mode-btn"><span class="btn-label">Hose 1</span><span class="btn-sub">Standby!</span></button>
          <button id="hose2EnableBtn" class="mode-btn"><span class="btn-label">Hose 2</span><span class="btn-sub">Standby!</span></button>
        </div>
      </div>

      <div class="gauge-card large right">
        <div class="gauge-header">Resin Pressure</div>
        <canvas id="resinGauge" class="gauge"></canvas>
        <div class="gauge-value" id="resinValue">0 PSI</div>
        <div class="gauge-value secondary" id="resinTempValue">-- °F</div>
      </div>
    </div>

    <!-- Bottom row: smaller low-pressure & air gauges -->
<div class="bottom-row">
  <div class="gauge-card small" id="isoLowCard">
    <div class="gauge-header">Iso Low</div>
    <canvas id="isoLowGauge" class="gauge"></canvas>
    <div class="gauge-value" id="isoLowValue">0 PSI</div>
    <div class="gauge-value secondary" id="isoLowTempValue">-- °F</div>
  </div>

  <div class="gauge-card small">
    <div class="gauge-header">Primary Air Piston</div>
    <canvas id="primaryAirGauge" class="gauge"></canvas>
    <div class="gauge-value" id="primaryAirValue">0 PSI</div>
  </div>

  <div class="gauge-card small setpoint-card" id="hose1SetCard">
    <button class="sp-btn sp-up" id="hose1SetUpBtn">▲</button>
    <div class="sp-center">
      <div class="sp-label">Hose Heat 1</div>
      <div class="sp-value" id="hose1SetValue">-- °F</div>
    </div>
    <button class="sp-btn sp-down" id="hose1SetDownBtn">▼</button>
  </div>

  <div class="gauge-card small setpoint-card" id="hose2SetCard">
    <button class="sp-btn sp-up" id="hose2SetUpBtn">▲</button>
    <div class="sp-center">
      <div class="sp-label">Hose Heat 2</div>
      <div class="sp-value" id="hose2SetValue">-- °F</div>
    </div>
    <button class="sp-btn sp-down" id="hose2SetDownBtn">▼</button>
  </div>

  <div class="gauge-card small">
    <div class="gauge-header">Gun AP Air</div>
    <canvas id="gunAirGauge" class="gauge"></canvas>
    <div class="gauge-value" id="gunAirValue">0 PSI</div>
  </div>

  <div class="gauge-card small" id="resinLowCard">
    <div class="gauge-header">Resin Low</div>
    <canvas id="resinLowGauge" class="gauge"></canvas>
    <div class="gauge-value" id="resinLowValue">0 PSI</div>
    <div class="gauge-value secondary" id="resinLowTempValue">-- °F</div>
  </div>
</div>
</main>

  <footer>
    <div>Last update: <span id="lastUpdate">--:--:--</span></div>
    <div class="footer-right">{{FW_VERSION}} • V2: 2-zone hose heat (setpoint + swing) + JSON telemetry + relay interlock + OTA</div>
  </footer>
</div>

<script>
  var target = 1000;
  var margin = 10;
  var diff   = 50;

  var airTarget = 100;
  var gunTarget = 100;
  var isoLowTarget   = 200;
  var resinLowTarget = 200;

  var supplyLow = 150;

  // Temperature configuration
  var isoTempTarget      = 120; // HP Iso
  var resinTempTarget    = 120; // HP Resin
  var isoLowTempTarget   = 120; // Low Iso
  var resinLowTempTarget = 120; // Low Resin
  var tempMin            = 40;
  var tempMax            = 180;

  var isoTemp      = null;
  var resinTemp    = null;
  var isoLowTemp   = null;
  var resinLowTemp = null;

  var drumAir = false;
  var spray   = false;

  // Hose heat (live page)
  var hose1En   = false;
  var hose2En   = false;
  var hose1Heat = false;
  var hose2Heat = false;
  var hose1Temp = null;
  var hose2Temp = null;
  var hose1Set  = null;
  var hose2Set  = null;
  var hose1Tol  = null;
  var hose2Tol  = null;

  var lastIsoLow   = 0;
  var lastResinLow = 0;

  var interlockActive = false;
  var resetInFlight = false;

  function updateResetButton() {
    var btn = document.getElementById('interlockResetBtn');
    if (!btn) return;
    if (interlockActive) btn.classList.remove('is-hidden');
    else btn.classList.add('is-hidden');
    btn.disabled = resetInFlight;
  }

  function requestInterlockReset() {
    if (!interlockActive || resetInFlight) return;
    resetInFlight = true;
    updateResetButton();
    fetch('/api/interlock/reset', { method: 'POST' })
      .then(function(r){ return r.json(); })
      .then(function(res){
        if (res && res.ok) {
          interlockActive = false;
          setStatus('Ready', false);
        } else {
          var msg = (res && (res.error || res.message)) ? (res.error || res.message) : 'Interlock reset denied';
          setStatus(msg, true);
          interlockActive = true; // remain latched visually
        }
        updateResetButton();
      })
      .catch(function(){
        setStatus('Interlock reset failed', true);
        interlockActive = true;
        updateResetButton();
      })
      .finally(function(){
        resetInFlight = false;
        updateResetButton();
      });
  }


  // Persistent status bar helper
  function setStatus(msg, isError) {
    var bar = document.getElementById('alertBar');
    if (!msg) msg = 'Ready';
    bar.textContent = msg;

    if (isError) {
      bar.classList.add('alert-active');
    } else {
      bar.classList.remove('alert-active');
    }
  }

  // Initial status
  setStatus('Ready', false);
  // Interlock reset button
  (function(){
    var btn = document.getElementById('interlockResetBtn');
    if (btn) btn.addEventListener('click', requestInterlockReset);
    updateResetButton();
  })();


  // Load settings from device
  fetch('/api/settings').then(function(r){return r.json();}).then(function(data){
    target = data.target;
    margin = data.margin;
    diff   = data.diff;
    if (data.airTarget !== undefined)      airTarget      = data.airTarget;
    if (data.gunTarget !== undefined)      gunTarget      = data.gunTarget;
    if (data.isoLowTarget !== undefined)   isoLowTarget   = data.isoLowTarget;
    if (data.resinLowTarget !== undefined) resinLowTarget = data.resinLowTarget;
    if (data.supplyLow !== undefined)      supplyLow      = data.supplyLow;

    if (data.isoTempTarget !== undefined)      isoTempTarget      = data.isoTempTarget;
    if (data.resinTempTarget !== undefined)    resinTempTarget    = data.resinTempTarget;
    if (data.isoLowTempTarget !== undefined)   isoLowTempTarget   = data.isoLowTempTarget;
    if (data.resinLowTempTarget !== undefined) resinLowTempTarget = data.resinLowTempTarget;
    if (data.tempMinF !== undefined)           tempMin            = data.tempMinF;
    if (data.tempMaxF !== undefined)           tempMax            = data.tempMaxF;

    // Pressure gauge setpoints
    isoGauge.setPoint        = target;
    resinGauge.setPoint      = target;
    primaryAirGauge.setPoint = airTarget;
    gunAirGauge.setPoint     = gunTarget;
    isoLowGauge.setPoint     = isoLowTarget;
    resinLowGauge.setPoint   = resinLowTarget;

    // Temperature ring configuration on all temp-enabled gauges
    isoGauge.tempMin      = tempMin;
    isoGauge.tempMax      = tempMax;
    resinGauge.tempMin    = tempMin;
    resinGauge.tempMax    = tempMax;
    isoLowGauge.tempMin   = tempMin;
    isoLowGauge.tempMax   = tempMax;
    resinLowGauge.tempMin = tempMin;
    resinLowGauge.tempMax = tempMax;

    // Per-gauge temperature setpoints (drive green bands)
    isoGauge.tempSetPoint      = isoTempTarget;
    resinGauge.tempSetPoint    = resinTempTarget;
    isoLowGauge.tempSetPoint   = isoLowTempTarget;
    resinLowGauge.tempSetPoint = resinLowTempTarget;

    isoGauge.draw(isoGauge.value || 0);
    resinGauge.draw(resinGauge.value || 0);
    primaryAirGauge.draw(primaryAirGauge.value || 0);
    gunAirGauge.draw(gunAirGauge.value || 0);
    isoLowGauge.draw(isoLowGauge.value || 0);
    resinLowGauge.draw(resinLowGauge.value || 0);
  }).catch(function(e){ console.log('Settings load failed', e); });

  // Round gauge class with optional inner temp ring
  function RoundGauge(canvasId, maxVal) {
    this.canvas = document.getElementById(canvasId);
    this.ctx    = this.canvas.getContext('2d');
    this.max    = maxVal;
    this.value  = null;   // smoothed PSI value we draw
    this.size   = 0;
    this.setPoint = target;

    // Temperature ring config
    this.showTempRing = false;
    this.tempValue    = null;   // °F
    this.tempMin      = 40;
    this.tempMax      = 180;
    this.tempSetPoint = null;   // °F

    var self = this;
    this.resize = function() {
      var parentWidth = self.canvas.parentElement.clientWidth;
      var size = parentWidth;
      if (size > 260) size = 260;
      if (size < 160) size = 160;
      self.size = size;

      var scale = window.devicePixelRatio || 1;
      self.canvas.style.width  = size + 'px';
      self.canvas.style.height = size + 'px';
      self.canvas.width  = size * scale;
      self.canvas.height = size * scale;
      self.ctx.setTransform(scale, 0, 0, scale, 0, 0);
      self.draw(self.value !== null ? self.value : 0);
    };

    window.addEventListener('resize', this.resize);
    this.resize();
  }

  // Smooth PSI motion so the needle glides instead of jumping
  RoundGauge.prototype.setValue = function(v) {
    if (v < 0) v = 0;
    if (v > this.max) v = this.max;

    var alpha = 0.30;  // 0..1 (higher = more responsive, lower = smoother)
    if (this.value === null || isNaN(this.value)) {
      this.value = v;
    } else {
      this.value = this.value + alpha * (v - this.value);
    }
    this.draw(this.value);
  };

  RoundGauge.prototype.setTemp = function(tF) {
    this.tempValue = tF;
    if (this.showTempRing) {
      this.draw(this.value !== null ? this.value : 0);
    }
  };

  RoundGauge.prototype.draw = function(value) {
    var canvas = this.canvas;
    var ctx    = this.ctx;
    var w      = this.size;
    var h      = this.size;
    var cx     = w / 2;
    var cy     = h / 2 + 6;
    var r      = Math.min(w, h) / 2 - 20;
    var start  = 0.75 * Math.PI;
    var span   = 1.5 * Math.PI;
    var max    = this.max;

    if (value == null) value = 0;
    if (value < 0)     value = 0;
    if (value > max)   value = max;

    var mPct = margin / 100.0;
    var sp = (this.setPoint !== undefined && this.setPoint !== null) ? this.setPoint : target;
    var low  = Math.max(0, sp * (1 - mPct));
    var high = Math.min(max, sp * (1 + mPct));
    var a0   = start;
    var aMax = start + span;
    var aLow = start + (low  / max) * span;
    var aHigh= start + (high / max) * span;
    var aT   = start + (sp   / max) * span;

    ctx.clearRect(0, 0, w, h);

    // Outer PSI band: red/green segments
    if (low > 0) {
      ctx.beginPath();
      ctx.arc(cx, cy, r, a0, aLow);
      ctx.lineWidth = 8;
      ctx.strokeStyle = '#F00';
      ctx.stroke();
    }
    if (low < high) {
      ctx.beginPath();
      ctx.arc(cx, cy, r, aLow, aHigh);
      ctx.lineWidth = 8;
      ctx.strokeStyle = '#0F0';
      ctx.stroke();
    }
    if (high < max) {
      ctx.beginPath();
      ctx.arc(cx, cy, r, aHigh, aMax);
      ctx.lineWidth = 8;
      ctx.strokeStyle = '#F00';
      ctx.stroke();
    }

    // Outer PSI target line
    ctx.beginPath();
    ctx.moveTo(cx + Math.cos(aT) * (r - 18), cy + Math.sin(aT) * (r - 18));
    ctx.lineTo(cx + Math.cos(aT) * r,        cy + Math.sin(aT) * r);
    ctx.lineWidth = 3.5;
    ctx.strokeStyle = '#00F';
    ctx.stroke();

    // Inner temperature ring, deliberately pulled inward so it stays visually
    // separate from the pressure band. The green band is drawn first and
    // *not* overwritten by the temp arc (which is drawn on a smaller radius).
    if (this.showTempRing) {
      var tMin = this.tempMin;
      var tMax = this.tempMax;
      if (tMax <= tMin) {
        tMin = 40; tMax = 180;
      }

      // Temp ring closer to center than outer PSI band
      var innerR = r - 15;

      // Base temp range background
      ctx.beginPath();
      ctx.arc(cx, cy, innerR, start, start + span);
      ctx.lineWidth = 3;
      ctx.strokeStyle = '#1e293b';
      ctx.stroke();

      // Green temp band around tempSetPoint using same % margin
      if (this.tempSetPoint !== null && typeof this.tempSetPoint === 'number') {
        var spT = this.tempSetPoint;
        var mT  = mPct;

        var halfBand = Math.abs(spT) * mT;
        var lowT  = spT - halfBand;
        var highT = spT + halfBand;

        if (lowT  < tMin) lowT  = tMin;
        if (highT > tMax) highT = tMax;

        if (highT > lowT) {
          var lowF  = (lowT  - tMin) / (tMax - tMin);
          var highF = (highT - tMin) / (tMax - tMin);
          var aLowT  = start + lowF  * span;
          var aHighT = start + highF * span;

          ctx.beginPath();
          ctx.arc(cx, cy, innerR, aLowT, aHighT);
          ctx.lineWidth = 4;
          ctx.strokeStyle = '#22c55e';
          ctx.stroke();
        }
      }

      // Temperature tick marks & labels INSIDE the ring
      var tempTicks = 5; // endpoints + intermediates
      for (var ti = 0; ti < tempTicks; ti++) {
        var tf = ti / (tempTicks - 1);         // 0..1 across temp range
        var ta = start + tf * span;            // angle along arc

        var tInner = innerR - 4;
        var tOuter = innerR + 2;

        // Tick line
        ctx.beginPath();
        ctx.moveTo(cx + Math.cos(ta) * tInner, cy + Math.sin(ta) * tInner);
        ctx.lineTo(cx + Math.cos(ta) * tOuter, cy + Math.sin(ta) * tOuter);
        ctx.lineWidth = 1.5;
        ctx.strokeStyle = '#64748b';
        ctx.stroke();

        // Label every tick, inside the ring
        var tVal = Math.round(tMin + tf * (tMax - tMin));
        ctx.font = Math.round(w * 0.035) + 'px Arial';
        ctx.fillStyle = '#9ca3af';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        var labelR = innerR - 16; // inside the temp ring
        ctx.fillText(
          tVal + '°',
          cx + Math.cos(ta) * labelR,
          cy + Math.sin(ta) * labelR
        );
      }

      // Actual temperature: blue arc on a *smaller* radius so the green band
      // remains visible, plus a mini pointer crossing the ring.
      if (this.tempValue !== null && typeof this.tempValue === 'number') {
        var t = this.tempValue;
        if (t < tMin) t = tMin;
        if (t > tMax) t = tMax;
        var tFrac  = (t - tMin) / (tMax - tMin);
        var tAngle = start + tFrac * span;

        var tempR = innerR - 5; // smaller radius than green band

        // Blue temp arc
        ctx.beginPath();
        ctx.arc(cx, cy, tempR, start, tAngle);
        ctx.lineWidth = 3;
        ctx.strokeStyle = '#38bdf8';
        ctx.stroke();

        // Mini pointer crossing the full temp ring
        var pinInner = tempR - 6;
        var pinOuter = innerR + 6;
        ctx.beginPath();
        ctx.moveTo(cx + Math.cos(tAngle) * pinInner, cy + Math.sin(tAngle) * pinInner);
        ctx.lineTo(cx + Math.cos(tAngle) * pinOuter, cy + Math.sin(tAngle) * pinOuter);
        ctx.lineWidth = 2;
        ctx.strokeStyle = '#e5e7eb';
        ctx.stroke();
      }
    }

    // PSI ticks
    ctx.lineWidth = 2;
    ctx.strokeStyle = '#888';
    var ticks = (max <= 400) ? 9 : 17;
    for (var i = 0; i < ticks; i++) {
      var ta2 = start + (i / (ticks - 1)) * span;
      var tl = (i % 2 === 0) ? 12 : 6;
      var ts = r - tl;
      ctx.beginPath();
      ctx.moveTo(cx + Math.cos(ta2) * ts, cy + Math.sin(ta2) * ts);
      ctx.lineTo(cx + Math.cos(ta2) * r,  cy + Math.sin(ta2) * r);
      ctx.stroke();
      if (i % 2 === 0) {
        var lbl = Math.round((i / (ticks - 1)) * max);
        ctx.font = Math.round(w * 0.04) + 'px Arial';
        ctx.fillStyle = '#ddd';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(lbl, cx + Math.cos(ta2) * (r + 15), cy + Math.sin(ta2) * (r + 15));
      }
    }

    // PSI needle
    var ang = start + (value / max) * span;
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    ctx.lineTo(cx + Math.cos(ang) * (r - 10), cy + Math.sin(ang) * (r - 10));
    ctx.lineWidth = 4;
    ctx.strokeStyle = '#fff';
    ctx.stroke();
    // hub
    ctx.beginPath();
    ctx.arc(cx, cy, 4.5, 0, 2 * Math.PI);
    ctx.fillStyle = '#fff';
    ctx.fill();

    // PSI numeric text on face
    ctx.font = 'bold ' + Math.round(w * 0.06) + 'px Arial';
    ctx.fillStyle = '#e5e7eb';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(Math.round(value) + ' PSI', cx, cy + 34);
  };

  // Gauges
  var isoGauge        = new RoundGauge('isoGauge',        1600);
  var resinGauge      = new RoundGauge('resinGauge',      1600);
  var isoLowGauge     = new RoundGauge('isoLowGauge',      500);
  var resinLowGauge   = new RoundGauge('resinLowGauge',    500);
  var primaryAirGauge = new RoundGauge('primaryAirGauge',  200);
  var gunAirGauge     = new RoundGauge('gunAirGauge',      200);

  isoLowGauge.setPoint   = isoLowTarget;
  resinLowGauge.setPoint = resinLowTarget;

  // Enable inner temp rings on high-pressure and low-side gauges
  isoGauge.showTempRing      = true;
  resinGauge.showTempRing    = true;
  isoLowGauge.showTempRing   = true;
  resinLowGauge.showTempRing = true;

  function updateRatio(iso, resin) {
    var circle = document.getElementById('ratioCircle');
    if (iso < 50 && resin < 50) {
      circle.className = 'ratio-circle';
      return;
    }
    if (Math.abs(iso - resin) <= diff) {
      circle.className = 'ratio-circle ratio-good';
    } else {
      circle.className = 'ratio-circle ratio-bad';
    }
  }

  function updateModeButtons() {
    var drumBtn  = document.getElementById('drumAirBtn');
    var sprayBtn = document.getElementById('sprayBtn');

    // Drum air on/off
    if (drumAir) {
      drumBtn.classList.add('on');
    } else {
      drumBtn.classList.remove('on');
    }

    // Determine if spray *can* be turned on:
    //  - drumAir must be enabled
    //  - both low-side feeds must be above supplyLow
    //  - no interlock is latched
    var supplyOk = (lastIsoLow >= supplyLow && lastResinLow >= supplyLow);
    var canSpray = drumAir && supplyOk && !interlockActive;

    sprayBtn.classList.remove('on');
    sprayBtn.classList.remove('interlock');

    if (spray) {
      // Actively spraying: always green
      sprayBtn.classList.add('on');
    } else if (!canSpray) {
      // Cannot be turned on right now: show red styling
      sprayBtn.classList.add('interlock');
    }
  }


  function fmtTempF(v) {
    if (v === null || v === undefined) return '-- °F';
    var n = Number(v);
    if (!isFinite(n)) return '-- °F';
    return n.toFixed(1) + ' °F';
  }

  function fmtSetF(v) {
    if (v === null || v === undefined) return '-- °F';
    var n = Math.round(Number(v));
    if (!isFinite(n)) return '-- °F';
    return n + ' °F';
  }

  function normalizeHoseState(en, heat) {
    if (!en) return 'OFF';
    return heat ? 'HEATING!' : 'AT TEMP';
  }

  function updateHoseUI() {
    // Enable buttons
    var h1EnBtn = document.getElementById('hose1EnableBtn');
    var h2EnBtn = document.getElementById('hose2EnableBtn');

    if (h1EnBtn) {
      if (hose1En) h1EnBtn.classList.add('on'); else h1EnBtn.classList.remove('on');
      var sub = h1EnBtn.querySelector('.btn-sub');
      if (sub) sub.textContent = hose1En ? 'ON' : 'Standby!';
    }
    if (h2EnBtn) {
      if (hose2En) h2EnBtn.classList.add('on'); else h2EnBtn.classList.remove('on');
      var sub2 = h2EnBtn.querySelector('.btn-sub');
      if (sub2) sub2.textContent = hose2En ? 'ON' : 'Standby!';
    }

    // Status buttons
    var h1StatusBtn = document.getElementById('hose1StatusBtn');
    var h2StatusBtn = document.getElementById('hose2StatusBtn');
    if (h1StatusBtn) {
      // Green when actively heating, Blue when enabled and not heating ("At Temp")
      if (hose1En && hose1Heat) {
        h1StatusBtn.classList.add('heating');
        h1StatusBtn.classList.remove('attemp');
      } else {
        h1StatusBtn.classList.remove('heating');
        if (hose1En && isFinite(Number(hose1Temp))) h1StatusBtn.classList.add('attemp');
        else h1StatusBtn.classList.remove('attemp');
      }
      var t = document.getElementById('hose1TempDisplay');
      if (t) t.textContent = fmtTempF(hose1Temp);
      var s = document.getElementById('hose1StateDisplay');
      if (s) s.textContent = normalizeHoseState(hose1En, hose1Heat);
    }
    if (h2StatusBtn) {
      if (hose2En && hose2Heat) {
        h2StatusBtn.classList.add('heating');
        h2StatusBtn.classList.remove('attemp');
      } else {
        h2StatusBtn.classList.remove('heating');
        if (hose2En && isFinite(Number(hose2Temp))) h2StatusBtn.classList.add('attemp');
        else h2StatusBtn.classList.remove('attemp');
      }
      var t2 = document.getElementById('hose2TempDisplay');
      if (t2) t2.textContent = fmtTempF(hose2Temp);
      var s2 = document.getElementById('hose2StateDisplay');
      if (s2) s2.textContent = normalizeHoseState(hose2En, hose2Heat);
    }

    // Setpoint cards
    var sp1 = document.getElementById('hose1SetValue');
    var sp2 = document.getElementById('hose2SetValue');
    if (sp1) sp1.textContent = fmtSetF(hose1Set);
    if (sp2) sp2.textContent = fmtSetF(hose2Set);
  }

  function clampHoseSetF(v) {
    var n = Math.round(Number(v));
    if (!isFinite(n)) n = 120;
    // Keep within the same min/max bands we show on gauges
    if (n < tempMin) n = tempMin;
    if (n > tempMax) n = tempMax;
    return n;
  }

  function adjustHoseSet(which, delta) {
    var cur = (which === 1) ? hose1Set : hose2Set;
    if (cur === null || cur === undefined) cur = 120;
    var next = clampHoseSetF(Number(cur) + Number(delta));

    var payload = (which === 1) ? { hose1Set: next } : { hose2Set: next };

    // Optimistic update so UI feels responsive
    if (which === 1) hose1Set = next; else hose2Set = next;
    updateHoseUI();

    return sendControl(payload).then(function(body){
      if (which === 1 && body.hose1Set !== undefined) hose1Set = body.hose1Set;
      if (which === 2 && body.hose2Set !== undefined) hose2Set = body.hose2Set;
      updateHoseUI();
    }).catch(function(err){
      setStatus('Hose setpoint error: ' + err.message, true);
      throw err;
    });
  }

  function attachHold(btn, onTapDelta, onHoldDelta, which) {
    var holdT = null;
    var holdI = null;

    function clearTimers() {
      if (holdT) { clearTimeout(holdT); holdT = null; }
      if (holdI) { clearInterval(holdI); holdI = null; }
    }

    function start(e) {
      if (e) e.preventDefault();
      clearTimers();

      // Immediate single-step
      adjustHoseSet(which, onTapDelta);

      // After a short hold, begin repeating at 0.75s with 5-degree steps
      holdT = setTimeout(function(){
        holdI = setInterval(function(){
          adjustHoseSet(which, onHoldDelta);
        }, 750);
      }, 600);
    }

    function stop(e) {
      if (e) e.preventDefault();
      clearTimers();
    }

    btn.addEventListener('mousedown', start);
    btn.addEventListener('touchstart', start, { passive: false });

    btn.addEventListener('mouseup', stop);
    btn.addEventListener('mouseleave', stop);
    btn.addEventListener('touchend', stop);
    btn.addEventListener('touchcancel', stop);
  }



  function updateGauges(data) {
    var iso      = data.iso      || 0;
    var resin    = data.resin    || 0;
    var isoLow   = data.isoLow   || 0;
    var resinLow = data.resinLow || 0;
    var air      = data.airPiston || 0;
    var apAir    = data.apAir     || 0;

    if (data.isoTemp !== undefined)      isoTemp      = data.isoTemp;
    if (data.resinTemp !== undefined)    resinTemp    = data.resinTemp;
    if (data.isoLowTemp !== undefined)   isoLowTemp   = data.isoLowTemp;
    if (data.resinLowTemp !== undefined) resinLowTemp = data.resinLowTemp;
    if (typeof data.drumAir !== 'undefined') drumAir = !!data.drumAir;
    if (typeof data.spray   !== 'undefined') spray   = !!data.spray;

    // Hose heat fields (if present)
    if (typeof data.hose1En  !== 'undefined') hose1En  = !!data.hose1En;
    if (typeof data.hose2En  !== 'undefined') hose2En  = !!data.hose2En;
    if (typeof data.hose1Heat!== 'undefined') hose1Heat= !!data.hose1Heat;
    if (typeof data.hose2Heat!== 'undefined') hose2Heat= !!data.hose2Heat;

    if (data.hose1Temp !== undefined) hose1Temp = data.hose1Temp;
    if (data.hose2Temp !== undefined) hose2Temp = data.hose2Temp;
    if (data.hose1Set  !== undefined) hose1Set  = data.hose1Set;
    if (data.hose2Set  !== undefined) hose2Set  = data.hose2Set;
    if (data.hose1Tol  !== undefined) hose1Tol  = data.hose1Tol;
    if (data.hose2Tol  !== undefined) hose2Tol  = data.hose2Tol;

    lastIsoLow   = isoLow;
    lastResinLow = resinLow;

    // Interlock message from backend (only when key present)
    if (typeof data.interlock !== 'undefined') {
      if (data.interlock) {
        interlockActive = true;
        setStatus(data.interlock, true);
      } else {
        interlockActive = false;
        var bar = document.getElementById('alertBar');
        setStatus(bar.textContent || 'Ready', false);
      }
    }

    updateResetButton();

    isoGauge.setValue(iso);
    resinGauge.setValue(resin);
    isoLowGauge.setValue(isoLow);
    resinLowGauge.setValue(resinLow);
    primaryAirGauge.setValue(air);
    gunAirGauge.setValue(apAir);

    if (isoTemp !== null)        isoGauge.setTemp(isoTemp);
    if (resinTemp !== null)      resinGauge.setTemp(resinTemp);
    if (isoLowTemp !== null)     isoLowGauge.setTemp(isoLowTemp);
    if (resinLowTemp !== null)   resinLowGauge.setTemp(resinLowTemp);

    document.getElementById('isoValue').textContent        = iso.toFixed(0) + ' PSI';
    document.getElementById('resinValue').textContent      = resin.toFixed(0) + ' PSI';
    document.getElementById('isoLowValue').textContent     = isoLow.toFixed(0) + ' PSI';
    document.getElementById('resinLowValue').textContent   = resinLow.toFixed(0) + ' PSI';
    document.getElementById('primaryAirValue').textContent = air.toFixed(0) + ' PSI';
    document.getElementById('gunAirValue').textContent     = apAir.toFixed(0) + ' PSI';

    // Temperature readouts
    var isoTempEl       = document.getElementById('isoTempValue');
    var resinTempEl     = document.getElementById('resinTempValue');
    var isoLowTempEl    = document.getElementById('isoLowTempValue');
    var resinLowTempEl  = document.getElementById('resinLowTempValue');

    if (isoTempEl) {
      isoTempEl.textContent =
        (isoTemp !== null && isoTemp !== undefined)
          ? isoTemp.toFixed(1) + ' °F'
          : '-- °F';
    }
    if (resinTempEl) {
      resinTempEl.textContent =
        (resinTemp !== null && resinTemp !== undefined)
          ? resinTemp.toFixed(1) + ' °F'
          : '-- °F';
    }
    if (isoLowTempEl) {
      isoLowTempEl.textContent =
        (isoLowTemp !== null && isoLowTemp !== undefined)
          ? isoLowTemp.toFixed(1) + ' °F'
          : '-- °F';
    }
    if (resinLowTempEl) {
      resinLowTempEl.textContent =
        (resinLowTemp !== null && resinLowTemp !== undefined)
          ? resinLowTemp.toFixed(1) + ' °F'
          : '-- °F';
    }

    updateRatio(iso, resin);
    updateModeButtons();
    updateHoseUI();

    var now = new Date();
    document.getElementById('lastUpdate').textContent =
      now.toLocaleTimeString([], { hour12: false });
  }

  function setLiveStatus(ok) {
    var text = document.getElementById('liveStatusText');
    var dot  = document.getElementById('liveDot');
    if (ok) {
      text.textContent = 'Live';
      dot.style.backgroundColor = '#22c55e';
      dot.style.boxShadow = '0 0 8px rgba(34,197,94,0.8)';
    } else {
      text.textContent = 'Offline';
      dot.style.backgroundColor = '#ef4444';
      dot.style.boxShadow = '0 0 8px rgba(239,68,68,0.8)';
    }
  }

  function sendControl(payload) {
    return fetch('/api/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    }).then(function(r){
      return r.json().then(function(body){
        if (!r.ok || body.ok === false) {
          throw new Error(body.error || ('HTTP ' + r.status));
        }
        if (typeof body.drumAir !== 'undefined') drumAir = !!body.drumAir;
        if (typeof body.spray   !== 'undefined') spray   = !!body.spray;

        if (typeof body.hose1En  !== 'undefined') hose1En  = !!body.hose1En;
        if (typeof body.hose2En  !== 'undefined') hose2En  = !!body.hose2En;
        if (typeof body.hose1Heat!== 'undefined') hose1Heat= !!body.hose1Heat;
        if (typeof body.hose2Heat!== 'undefined') hose2Heat= !!body.hose2Heat;
        if (body.hose1Temp !== undefined) hose1Temp = body.hose1Temp;
        if (body.hose2Temp !== undefined) hose2Temp = body.hose2Temp;
        if (body.hose1Set  !== undefined) hose1Set  = body.hose1Set;
        if (body.hose2Set  !== undefined) hose2Set  = body.hose2Set;
        if (body.hose1Tol  !== undefined) hose1Tol  = body.hose1Tol;
        if (body.hose2Tol  !== undefined) hose2Tol  = body.hose2Tol;

        if (typeof body.interlock !== 'undefined' && body.interlock) {
          // Interlock still active
          interlockActive = true;
          setStatus(body.interlock, true);
        } else {
          // Successful command with no interlock -> clear the red state
          interlockActive = false;
        }

        updateModeButtons();
        updateHoseUI();
        return body;
      });
    });
  }

  document.getElementById('drumAirBtn').addEventListener('click', function(){
    var desired = !drumAir;

    sendControl({ drumAir: desired }).then(function(body){
      var msg = desired ? 'Drum air enabled' : 'Drum air disabled';
      setStatus(msg, false);
    }).catch(function(err){
      console.log('DrumAir control failed:', err);
      setStatus('Drum air error: ' + err.message, true);
    });
  });

  document.getElementById('sprayBtn').addEventListener('click', function(){
    var desired = !spray;

    sendControl({ spray: desired }).then(function(body){
      var msg = desired ? 'Spray enabled' : 'Spray disabled';
      setStatus(msg, false);
    }).catch(function(err){
      console.log('Spray control failed:', err);
      setStatus('Spray error: ' + err.message, true);
    });
  });

  
  // Hose heat enable controls
  var hose1EnableBtn = document.getElementById('hose1EnableBtn');
  var hose2EnableBtn = document.getElementById('hose2EnableBtn');

  if (hose1EnableBtn) {
    hose1EnableBtn.addEventListener('click', function(){
      var desired = !hose1En;
      // optimistic
      hose1En = desired;
      updateHoseUI();

      sendControl({ hose1En: desired }).then(function(){
        setStatus(desired ? 'Hose 1 enabled' : 'Hose 1 standby', false);
      }).catch(function(err){
        setStatus('Hose 1 enable error: ' + err.message, true);
      });
    });
  }

  if (hose2EnableBtn) {
    hose2EnableBtn.addEventListener('click', function(){
      var desired = !hose2En;
      // optimistic
      hose2En = desired;
      updateHoseUI();

      sendControl({ hose2En: desired }).then(function(){
        setStatus(desired ? 'Hose 2 enabled' : 'Hose 2 standby', false);
      }).catch(function(err){
        setStatus('Hose 2 enable error: ' + err.message, true);
      });
    });
  }

  // Hose heat setpoint (press: 1°F, hold: 5°F every 0.75s)
  var hose1Up = document.getElementById('hose1SetUpBtn');
  var hose1Dn = document.getElementById('hose1SetDownBtn');
  var hose2Up = document.getElementById('hose2SetUpBtn');
  var hose2Dn = document.getElementById('hose2SetDownBtn');

  if (hose1Up) attachHold(hose1Up, +1, +5, 1);
  if (hose1Dn) attachHold(hose1Dn, -1, -5, 1);
  if (hose2Up) attachHold(hose2Up, +1, +5, 2);
  if (hose2Dn) attachHold(hose2Dn, -1, -5, 2);


  // Shared WebSocket instance with auto-reconnect
  var ws = null;

  function connectWS() {
    // Avoid creating multiple sockets if one is already open/connecting
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
      return;
    }

    var scheme = (location.protocol === 'https:') ? 'wss://' : 'ws://';
    ws = new WebSocket(scheme + location.hostname + ':81/');

    ws.onopen = function () {
      console.log('WS connected');
      setLiveStatus(true);
    };

    ws.onmessage = function (ev) {
      try {
        var d = JSON.parse(ev.data);
        updateGauges(d);
      } catch (e) {
        console.log('Bad WS payload', e);
      }
    };

    ws.onclose = function () {
      console.log('WS closed, scheduling reconnect');
      setLiveStatus(false);
      ws = null;
      setTimeout(connectWS, 1500);
    };

    ws.onerror = function (err) {
      console.log('WS error', err);
      setLiveStatus(false);
      try { ws.close(); } catch (e) {}
    };
  }

  // Initial fetch of relay state
  fetch('/api/control').then(function(r){ return r.json(); }).then(function(data){
    if (typeof data.drumAir !== 'undefined') drumAir = !!data.drumAir;
    if (typeof data.spray   !== 'undefined') spray   = !!data.spray;

    if (typeof data.hose1En  !== 'undefined') hose1En  = !!data.hose1En;
    if (typeof data.hose2En  !== 'undefined') hose2En  = !!data.hose2En;
    if (typeof data.hose1Heat!== 'undefined') hose1Heat= !!data.hose1Heat;
    if (typeof data.hose2Heat!== 'undefined') hose2Heat= !!data.hose2Heat;
    if (data.hose1Temp !== undefined) hose1Temp = data.hose1Temp;
    if (data.hose2Temp !== undefined) hose2Temp = data.hose2Temp;
    if (data.hose1Set  !== undefined) hose1Set  = data.hose1Set;
    if (data.hose2Set  !== undefined) hose2Set  = data.hose2Set;
    if (data.hose1Tol  !== undefined) hose1Tol  = data.hose1Tol;
    if (data.hose2Tol  !== undefined) hose2Tol  = data.hose2Tol;

    updateModeButtons();
    updateHoseUI();
  }).catch(function(e){
    console.log('Failed to load relay state:', e);
  });

  window.addEventListener('load', connectWS);
</script>
</body>
</html>
)rawliteral";

// ---------- SETTINGS PAGE ----------

const char* settingsPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>Spray Foam Settings</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <meta name="apple-mobile-web-app-capable" content="yes" />
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent" />
  <style>
    :root {
      --bg: #020617;
      --panel: #020617;
      --panel-border: #1e293b;
      --accent: #38bdf8;
      --text-main: #e5e7eb;
      --text-muted: #9ca3af;
    }
    * { box-sizing: border-box; -webkit-font-smoothing: antialiased; }
    body {
      margin: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont,
        "SF Pro Text", "Segoe UI", sans-serif;
      background: radial-gradient(circle at top, #1e293b 0, #020617 55%, #000 100%);
      color: var(--text-main);
      min-height: 100vh;
    }
    .app {
      max-width: 960px;
      margin: 0 auto;
      padding: 0.75rem 1.25rem 1.5rem;
    }
    header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 0.75rem;
    }
    h1 {
      margin: 0;
      font-size: 1.35rem;
    }
    .subtitle {
      font-size: 0.8rem;
      color: var(--text-muted);
    }
    .nav-buttons {
      display:flex;
      gap:0.4rem;
    }
    .nav-btn {
      border-radius: 999px;
      padding: 0.4rem 0.9rem;
      border: 1px solid rgba(148,163,184,0.6);
      background: rgba(15,23,42,0.95);
      color: var(--text-main);
      text-decoration: none;
      font-size: 0.85rem;
    }
    form {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 0.9rem;
    }
    fieldset {
      border-radius: 16px;
      border: 1px solid var(--panel-border);
      background: var(--panel);
      padding: 0.9rem 1rem 1rem;
      min-width: 0;
    }
    legend {
      font-size: 0.85rem;
      text-transform: uppercase;
      letter-spacing: 0.12em;
      color: var(--text-muted);
      padding: 0 0.3rem;
    }
    .field {
      display: flex;
      flex-direction: column;
      margin-bottom: 0.6rem;
    }
    label {
      font-size: 0.8rem;
      color: var(--text-muted);
      margin-bottom: 0.15rem;
    }
    input[type="number"], input[type="text"], input[type="password"], select {
      padding: 0.35rem 0.5rem;
      border-radius: 8px;
      border: 1px solid #4b5563;
      background: #020617;
      color: var(--text-main);
      font-size: 0.85rem;
    }
    .hint {
      font-size: 0.7rem;
      color: var(--text-muted);
      margin-top: 0.15rem;
    }
    .full-width {
      grid-column: 1 / -1;
    }
    .save-bar {
      grid-column: 1 / -1;
      margin-top: 0.5rem;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 0.75rem;
    }
    .save-btn {
      flex: 0 0 auto;
      padding: 0.55rem 1.4rem;
      border-radius: 999px;
      border: none;
      background: linear-gradient(135deg, #38bdf8, #0ea5e9);
      color: #020617;
      font-weight: 600;
      font-size: 0.9rem;
    }
    .save-btn:active { transform: translateY(1px); }
    #saveStatus {
      font-size: 0.8rem;
      color: var(--text-muted);
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.75rem;
      margin-top: 0.4rem;
    }
    th, td {
      border: 1px solid #1f2937;
      padding: 4px 6px;
      text-align: left;
    }
    .ok { color: #22c55e; font-weight: 600; }
    .warn { color: #facc15; font-weight: 600; }
    .bad { color: #ef4444; font-weight: 600; }
    @media (max-width: 800px) {
      form { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
<div class="app">
  <header>
    <div>
      <h1>System Settings</h1>
      <div class="subtitle">
        Pressure ranges, ratio thresholds, temps &amp; calibration
        • Firmware <strong>{{FW_VERSION}}</strong>
      </div>
    </div>
    <div class="nav-buttons">
      <a class="nav-btn" href="/update">OTA Update</a>
      <a class="nav-btn" href="/">⬅ Back to Live</a>
    </div>
  </header>
  <form id="settingsForm">

    <fieldset>
      <legend>High-Pressure &amp; Ratio</legend>
      <div class="field">
        <label for="targetInput">Target Pressure (PSI)</label>
        <input type="number" id="targetInput" name="target" min="0" max="1600" />
        <div class="hint">Nominal operating pressure for Iso/Resin.</div>
      </div>
      <div class="field">
        <label for="marginInput">Margin (%)</label>
        <input type="number" id="marginInput" name="margin" min="0" max="100" />
        <div class="hint">Green band width around target (used for PSI &amp; temp bands).</div>
      </div>
      <div class="field">
        <label for="diffInput">Max Iso/Resin Difference (PSI)</label>
        <input type="number" id="diffInput" name="diff" min="0" max="1600" />
        <div class="hint">Used for ratio indicator and quick checks.</div>
      </div>
    </fieldset>

    <fieldset>
      <legend>Air &amp; Low-Side Pressure</legend>
      <div class="field">
        <label for="airTargetInput">Air Piston Set Point (PSI)</label>
        <input type="number" id="airTargetInput" name="airTarget" min="0" max="200" />
        <div class="hint">Target pressure for the Primary Air gauge (0–200&nbsp;PSI).</div>
      </div>
      <div class="field">
        <label for="gunTargetInput">Gun Air Set Point (PSI)</label>
        <input type="number" id="gunTargetInput" name="gunTarget" min="0" max="200" />
        <div class="hint">Target pressure for the Gun AP gauge (0–200&nbsp;PSI).</div>
      </div>
      <div class="field">
        <label for="isoLowTargetInput">Iso Low Set Point (PSI)</label>
        <input type="number" id="isoLowTargetInput" name="isoLowTarget" min="0" max="500" />
        <div class="hint">Target for low-pressure Iso feed (0–500&nbsp;PSI).</div>
      </div>
      <div class="field">
        <label for="resinLowTargetInput">Resin Low Set Point (PSI)</label>
        <input type="number" id="resinLowTargetInput" name="resinLowTarget" min="0" max="500" />
        <div class="hint">Target for low-pressure Resin feed (0–500&nbsp;PSI).</div>
      </div>
      <div class="field">
        <label for="supplyLowInput">Supply Low Pressure (PSI)</label>
        <input type="number" id="supplyLowInput" name="supplyLow" min="0" max="500" />
        <div class="hint">
          Minimum low-side pressure (either Iso/Resin). Below this in Spray mode will park and cut drum air;
          must be above this to enter Spray.
        </div>
      </div>
    </fieldset>

    <fieldset>
      <legend>Temperature Targets</legend>
      <div class="field">
        <label for="isoTempTargetInput">Iso HP Temp Target (°F)</label>
        <input type="number" id="isoTempTargetInput" name="isoTempTarget" min="-40" max="300" />
        <div class="hint">Target Iso outlet temp; green band shown on HP Iso temp ring.</div>
      </div>
      <div class="field">
        <label for="resinTempTargetInput">Resin HP Temp Target (°F)</label>
        <input type="number" id="resinTempTargetInput" name="resinTempTarget" min="-40" max="300" />
        <div class="hint">Target Resin outlet temp; green band shown on HP Resin temp ring.</div>
      </div>
      <div class="field">
        <label for="isoLowTempTargetInput">Iso Low Temp Target (°F)</label>
        <input type="number" id="isoLowTempTargetInput" name="isoLowTempTarget" min="-40" max="300" />
        <div class="hint">Target temp for low-side Iso feed; drives green band on Iso Low gauge.</div>
      </div>
      <div class="field">
        <label for="resinLowTempTargetInput">Resin Low Temp Target (°F)</label>
        <input type="number" id="resinLowTempTargetInput" name="resinLowTempTarget" min="-40" max="300" />
        <div class="hint">Target temp for low-side Resin feed; drives green band on Resin Low gauge.</div>
      </div>
      <div class="field">
        <label for="tempMinInput">Temp Gauge Minimum (°F)</label>
        <input type="number" id="tempMinInput" name="tempMinF" min="-40" max="300" />
        <div class="hint">Lower bound for the temp ring (all four sensors share this scale).</div>
      </div>
      <div class="field">
        <label for="tempMaxInput">Temp Gauge Maximum (°F)</label>
        <input type="number" id="tempMaxInput" name="tempMaxF" min="-40" max="300" />
        <div class="hint">Upper bound for the temp ring (all four sensors share this scale).</div>
      </div>
    </fieldset>

    <fieldset>
      <legend>Hose Heat</legend>
      <div class="field">
        <label for="hose1SetInput">Hose Heat 1 Setpoint (°F)</label>
        <input type="number" id="hose1SetInput" name="hose1Set" min="-40" max="300" />
        <div class="hint">Target hose temperature for section 1.</div>
      </div>
      <div class="field">
        <label for="hose1TolInput">Hose Heat 1 Swing (°F)</label>
        <input type="number" id="hose1TolInput" name="hose1Tol" min="1" max="30" />
        <div class="hint">Heater turns ON at setpoint and OFF at (setpoint + swing).</div>
      </div>
      <div class="field">
        <label for="hose2SetInput">Hose Heat 2 Setpoint (°F)</label>
        <input type="number" id="hose2SetInput" name="hose2Set" min="-40" max="300" />
        <div class="hint">Target hose temperature for section 2.</div>
      </div>
      <div class="field">
        <label for="hose2TolInput">Hose Heat 2 Swing (°F)</label>
        <input type="number" id="hose2TolInput" name="hose2Tol" min="1" max="30" />
        <div class="hint">Heater turns ON at setpoint and OFF at (setpoint + swing).</div>
      </div>
    
      <div class="field">
        <label for="hoseOvertempInput">Hose Overtemp Cutoff (°F)</label>
        <input type="number" id="hoseOvertempInput" name="hoseOvertempF" min="1" max="50" />
        <div class="hint">Safety interlock: if a hose temp exceeds (setpoint + cutoff), all hose heaters shut down and the system is parked.</div>
      </div>
    </fieldset>

    <fieldset>
      <legend>Temperature Sensors</legend>
      <div class="field">
        <label for="isoTempSensorSelect">Iso HP Temp Sensor</label>
        <select id="isoTempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="isoTempReading">--</span> °F
          <br/>Assign a DS18B20 to the Iso HP outlet.
        </div>
      </div>
      <div class="field">
        <label for="resinTempSensorSelect">Resin HP Temp Sensor</label>
        <select id="resinTempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="resinTempReading">--</span> °F
          <br/>Assign a DS18B20 to the Resin HP outlet.
        </div>
      </div>
      <div class="field">
        <label for="isoLowTempSensorSelect">Iso Low Temp Sensor</label>
        <select id="isoLowTempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="isoLowTempReading">--</span> °F
          <br/>Assign a DS18B20 to the low-side Iso feed.
        </div>
      </div>
      <div class="field">
        <label for="resinLowTempSensorSelect">Resin Low Temp Sensor</label>
        <select id="resinLowTempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="resinLowTempReading">--</span> °F
          <br/>Assign a DS18B20 to the low-side Resin feed.
        </div>
      </div>
      <div class="field">
        <label for="hose1TempSensorSelect">Hose 1 Temp Sensor</label>
        <select id="hose1TempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="hose1TempReading">--</span> °F
          <br/>Assign a DS18B20 to Hose Heat Section 1.
        </div>
      </div>
      <div class="field">
        <label for="hose2TempSensorSelect">Hose 2 Temp Sensor</label>
        <select id="hose2TempSensorSelect"></select>
        <div class="hint">
          Current reading: <span id="hose2TempReading">--</span> °F
          <br/>Assign a DS18B20 to Hose Heat Section 2.
        </div>
      </div>
    </fieldset>


    <fieldset>
      <legend>Calibration Controls</legend>
      <div class="field">
        <label for="sensorSelect">Sensor</label>
        <select id="sensorSelect">
          <option value="iso">Iso (0–1600)</option>
          <option value="resin">Resin (0–1600)</option>
          <option value="isoLow">Iso Low (0–500)</option>
          <option value="resinLow">Resin Low (0–500)</option>
          <option value="air">Air Piston (0–300)</option>
          <option value="apAir">Gun AP Air (0–300)</option>
        </select>
      </div>
      <div class="field">
        <label>Zero at 0 PSI</label>
        <button type="button" id="zeroBtn">Zero Selected</button>
        <button type="button" id="zeroAllBtn">Zero All</button>
        <div class="hint">Put all lines at 0 PSI before using these.</div>
      </div>
      <div class="field">
        <label for="spanPressureInput">Known Pressure (PSI) for Span</label>
        <input type="number" id="spanPressureInput" min="0" max="2000" step="1" />
        <button type="button" id="spanBtn" style="margin-top:0.25rem;">Set Span</button>
        <div class="hint">Pressurize to a known value (e.g. 100 or 1000 PSI) then set span.</div>
      </div>
      <div id="calibStatus" class="hint"></div>
    </fieldset>

    <fieldset class="full-width">
      <legend>Network &amp; OTA</legend>
      <div class="field">
        <label for="wifiModeSelect">Network Mode</label>
        <select id="wifiModeSelect" name="wifiMode">
          <option value="0">Access Point (AP)</option>
          <option value="1">WiFi Client (STA)</option>
        </select>
        <div class="hint">
          AP: rig creates its own WiFi network.<br/>
          STA: rig joins an existing WiFi network. Changes take effect after reboot.
        </div>
      </div>
      <div class="field">
        <label for="apSsidInput">AP SSID</label>
        <input type="text" id="apSsidInput" />
        <div class="hint">SSID for the rig's hotspot.</div>
      </div>
      <div class="field">
        <label for="apPassInput">AP Password</label>
        <input type="password" id="apPassInput" />
        <div class="hint">Minimum 8 characters (WPA2).</div>
      </div>

      <div class="field">
        <label for="staSsidInput">WiFi (STA) SSID</label>
        <input type="text" id="staSsidInput" />
        <div class="hint">Existing WiFi network the rig should join.</div>
      </div>
      <div class="field">
        <label for="staPassInput">WiFi (STA) Password</label>
        <input type="password" id="staPassInput" />
        <div class="hint">WPA2 password for that network.</div>
      </div>

      <div class="hint">
        Tip: keep AP enabled until you're sure STA connects. You can always fall back by power-cycling and switching modes.
      </div>
    </fieldset>

    <fieldset class="full-width">
      <legend>Calibration Status</legend>
      <table>
        <thead>
          <tr><th>Sensor</th><th>R0 (raw @0)</th><th>K (scale)</th><th>Note</th></tr>
        </thead>
        <tbody id="statusTable"></tbody>
      </table>
    </fieldset>

    <div class="save-bar">
      <button type="submit" class="save-btn">Save &amp; Return to Live</button>
      <div id="saveStatus">Settings not saved yet.</div>
    </div>
  </form>
</div>
<script>
  var tempSensorMap = {};

  function updateTempReadouts() {
    var isoSel       = document.getElementById('isoTempSensorSelect');
    var resinSel     = document.getElementById('resinTempSensorSelect');
    var isoLowSel    = document.getElementById('isoLowTempSensorSelect');
    var resinLowSel  = document.getElementById('resinLowTempSensorSelect');
    var hose1Sel     = document.getElementById('hose1TempSensorSelect');
    var hose2Sel     = document.getElementById('hose2TempSensorSelect');

    var isoSpan      = document.getElementById('isoTempReading');
    var resinSpan    = document.getElementById('resinTempReading');
    var isoLowSpan   = document.getElementById('isoLowTempReading');
    var resinLowSpan = document.getElementById('resinLowTempReading');
    var hose1Span    = document.getElementById('hose1TempReading');
    var hose2Span    = document.getElementById('hose2TempReading');

    var isoId       = isoSel.value;
    var resinId     = resinSel.value;
    var isoLowId    = isoLowSel.value;
    var resinLowId  = resinLowSel.value;
    var hose1Id     = hose1Sel.value;
    var hose2Id     = hose2Sel.value;

    if (isoId && tempSensorMap.hasOwnProperty(isoId) && tempSensorMap[isoId] != null) {
      isoSpan.textContent = tempSensorMap[isoId].toFixed(1);
    } else {
      isoSpan.textContent = '--';
    }

    if (resinId && tempSensorMap.hasOwnProperty(resinId) && tempSensorMap[resinId] != null) {
      resinSpan.textContent = tempSensorMap[resinId].toFixed(1);
    } else {
      resinSpan.textContent = '--';
    }

    if (isoLowId && tempSensorMap.hasOwnProperty(isoLowId) && tempSensorMap[isoLowId] != null) {
      isoLowSpan.textContent = tempSensorMap[isoLowId].toFixed(1);
    } else {
      isoLowSpan.textContent = '--';
    }

    if (resinLowId && tempSensorMap.hasOwnProperty(resinLowId) && tempSensorMap[resinLowId] != null) {
      resinLowSpan.textContent = tempSensorMap[resinLowId].toFixed(1);
    } else {
      resinLowSpan.textContent = '--';
    }

    if (hose1Id && tempSensorMap.hasOwnProperty(hose1Id) && tempSensorMap[hose1Id] != null) {
      hose1Span.textContent = tempSensorMap[hose1Id].toFixed(1);
    } else {
      hose1Span.textContent = '--';
    }

    if (hose2Id && tempSensorMap.hasOwnProperty(hose2Id) && tempSensorMap[hose2Id] != null) {
      hose2Span.textContent = tempSensorMap[hose2Id].toFixed(1);
    } else {
      hose2Span.textContent = '--';
    }
  }

  // Load existing settings
  fetch('/api/settings').then(function(r){return r.json();}).then(function(data){
    document.getElementById('targetInput').value = data.target;
    document.getElementById('marginInput').value = data.margin;
    document.getElementById('diffInput').value   = data.diff;
    if (data.airTarget !== undefined) {
      document.getElementById('airTargetInput').value = data.airTarget;
    }
    if (data.gunTarget !== undefined) {
      document.getElementById('gunTargetInput').value = data.gunTarget;
    }
    if (data.isoLowTarget !== undefined) {
      document.getElementById('isoLowTargetInput').value = data.isoLowTarget;
    }
    if (data.resinLowTarget !== undefined) {
      document.getElementById('resinLowTargetInput').value = data.resinLowTarget;
    }
    if (data.supplyLow !== undefined) {
      document.getElementById('supplyLowInput').value = data.supplyLow;
    }
    if (data.isoTempTarget !== undefined) {
      document.getElementById('isoTempTargetInput').value = data.isoTempTarget;
    }
    if (data.resinTempTarget !== undefined) {
      document.getElementById('resinTempTargetInput').value = data.resinTempTarget;
    }
    if (data.isoLowTempTarget !== undefined) {
      document.getElementById('isoLowTempTargetInput').value = data.isoLowTempTarget;
    }
    if (data.resinLowTempTarget !== undefined) {
      document.getElementById('resinLowTempTargetInput').value = data.resinLowTempTarget;
    }
    if (data.tempMinF !== undefined) {
      document.getElementById('tempMinInput').value = data.tempMinF;
    }
    if (data.tempMaxF !== undefined) {
      document.getElementById('tempMaxInput').value = data.tempMaxF;
    }

    // Hose heat controls (setpoint + swing)
    if (data.hose1Set !== undefined) {
      document.getElementById('hose1SetInput').value = data.hose1Set;
    }
    if (data.hose2Set !== undefined) {
      document.getElementById('hose2SetInput').value = data.hose2Set;
    }
    if (data.hose1Tol !== undefined) {
      document.getElementById('hose1TolInput').value = data.hose1Tol;
    }
    if (data.hose2Tol !== undefined) {
      document.getElementById('hose2TolInput').value = data.hose2Tol;
    }

    if (data.hoseOvertempF !== undefined) {
      document.getElementById('hoseOvertempInput').value = data.hoseOvertempF;
    }

    if (data.wifiMode !== undefined) {
      document.getElementById('wifiModeSelect').value = data.wifiMode;
    }
    if (data.apSsid !== undefined) {
      document.getElementById('apSsidInput').value = data.apSsid;
    }
    if (data.apPass !== undefined) {
      document.getElementById('apPassInput').value = data.apPass;
    }
    if (data.staSsid !== undefined) {
      document.getElementById('staSsidInput').value = data.staSsid;
    }
    if (data.staPass !== undefined) {
      document.getElementById('staPassInput').value = data.staPass;
    }
  }).catch(function(e){
    document.getElementById('saveStatus').textContent = 'Failed to load settings: ' + e;
  });

  // Load temp sensors and assignments (initial only)
  function refreshTempSensors(initial) {
    var isoSel       = document.getElementById('isoTempSensorSelect');
    var resinSel     = document.getElementById('resinTempSensorSelect');
    var isoLowSel    = document.getElementById('isoLowTempSensorSelect');
    var resinLowSel  = document.getElementById('resinLowTempSensorSelect');
    var hose1Sel     = document.getElementById('hose1TempSensorSelect');
    var hose2Sel     = document.getElementById('hose2TempSensorSelect');

    fetch('/api/temp-sensors')
      .then(function(r){ return r.json(); })
      .then(function(data){
        tempSensorMap = {};

        var sensors = data.sensors || [];
        sensors.forEach(function(s){
          if (s.id) {
            tempSensorMap[s.id] = (typeof s.tempF === 'number') ? s.tempF : null;
          }
        });

        if (initial) {
          // Build select options once from current sensor list
          isoSel.innerHTML      = '';
          resinSel.innerHTML    = '';
          isoLowSel.innerHTML   = '';
          resinLowSel.innerHTML = '';
          hose1Sel.innerHTML    = '';
          hose2Sel.innerHTML    = '';

          var optNone1 = document.createElement('option');
          optNone1.value = '';
          optNone1.textContent = 'Unassigned';
          isoSel.appendChild(optNone1);

          var optNone2 = document.createElement('option');
          optNone2.value = '';
          optNone2.textContent = 'Unassigned';
          resinSel.appendChild(optNone2);

          var optNone3 = document.createElement('option');
          optNone3.value = '';
          optNone3.textContent = 'Unassigned';
          isoLowSel.appendChild(optNone3);

          var optNone4 = document.createElement('option');
          optNone4.value = '';
          optNone4.textContent = 'Unassigned';
          resinLowSel.appendChild(optNone4);

          var optNone5 = document.createElement('option');
          optNone5.value = '';
          optNone5.textContent = 'Unassigned';
          hose1Sel.appendChild(optNone5);

          var optNone6 = document.createElement('option');
          optNone6.value = '';
          optNone6.textContent = 'Unassigned';
          hose2Sel.appendChild(optNone6);

          sensors.forEach(function(s, idx){
            var label = 'Sensor ' + idx + ' (' + s.id + ')';
            if (typeof s.tempF === 'number') {
              label += ' – ' + s.tempF.toFixed(1) + ' °F';
            } else {
              label += ' – n/a';
            }

            [isoSel, resinSel, isoLowSel, resinLowSel, hose1Sel, hose2Sel].forEach(function(sel){
              var opt = document.createElement('option');
              opt.value = s.id;
              opt.textContent = label;
              sel.appendChild(opt);
            });
          });

          // Apply stored assignments from backend
          if (data.iso !== undefined && data.iso !== null) {
            isoSel.value = data.iso;
          }
          if (data.resin !== undefined && data.resin !== null) {
            resinSel.value = data.resin;
          }
          if (data.isoLow !== undefined && data.isoLow !== null) {
            isoLowSel.value = data.isoLow;
          }
          if (data.resinLow !== undefined && data.resinLow !== null) {
            resinLowSel.value = data.resinLow;
          }
          if (data.hose1 !== undefined && data.hose1 !== null) {
            hose1Sel.value = data.hose1;
          }
          if (data.hose2 !== undefined && data.hose2 !== null) {
            hose2Sel.value = data.hose2;
          }
        }

        // Always update readouts from latest temps
        updateTempReadouts();
      })
      .catch(function(e){
        console.log('Failed to load temp sensors:', e);
      });
  }

  refreshTempSensors(true);

  document.getElementById('isoTempSensorSelect')
    .addEventListener('change', updateTempReadouts);
  document.getElementById('resinTempSensorSelect')
    .addEventListener('change', updateTempReadouts);
  document.getElementById('isoLowTempSensorSelect')
    .addEventListener('change', updateTempReadouts);
  document.getElementById('resinLowTempSensorSelect')
    .addEventListener('change', updateTempReadouts);


  document.getElementById('hose1TempSensorSelect')
    .addEventListener('change', updateTempReadouts);
  document.getElementById('hose2TempSensorSelect')
    .addEventListener('change', updateTempReadouts);
  // Periodically refresh only the temperature values, not the assignments/options
  setInterval(function(){
    refreshTempSensors(false);
  }, 2000);

  document.getElementById('settingsForm').addEventListener('submit', function(e){
    e.preventDefault();
    var target = parseInt(document.getElementById('targetInput').value || '0', 10);
    var margin = parseInt(document.getElementById('marginInput').value || '0', 10);
    var diff   = parseInt(document.getElementById('diffInput').value   || '0', 10);

    var airTargetVal      = parseInt(document.getElementById('airTargetInput').value      || '0', 10);
    var gunTargetVal      = parseInt(document.getElementById('gunTargetInput').value      || '0', 10);
    var isoLowTargetVal   = parseInt(document.getElementById('isoLowTargetInput').value   || '0', 10);
    var resinLowTargetVal = parseInt(document.getElementById('resinLowTargetInput').value || '0', 10);
    var supplyLowVal      = parseInt(document.getElementById('supplyLowInput').value      || '0', 10);

    var isoTempTargetVal      = parseInt(document.getElementById('isoTempTargetInput').value      || '0', 10);
    var resinTempTargetVal    = parseInt(document.getElementById('resinTempTargetInput').value    || '0', 10);
    var isoLowTempTargetVal   = parseInt(document.getElementById('isoLowTempTargetInput').value   || '0', 10);
    var resinLowTempTargetVal = parseInt(document.getElementById('resinLowTempTargetInput').value || '0', 10);
    var tempMinVal            = parseInt(document.getElementById('tempMinInput').value            || '0', 10);
    var tempMaxVal            = parseInt(document.getElementById('tempMaxInput').value            || '0', 10);

    var hose1SetVal = parseInt(document.getElementById('hose1SetInput').value || '0', 10);
    var hose2SetVal = parseInt(document.getElementById('hose2SetInput').value || '0', 10);
    var hose1TolVal = parseInt(document.getElementById('hose1TolInput').value || '0', 10);
    var hose2TolVal = parseInt(document.getElementById('hose2TolInput').value || '0', 10);
    var hoseOvertempVal = parseInt(document.getElementById('hoseOvertempInput').value || '0', 10);

    var wifiModeVal = parseInt(document.getElementById('wifiModeSelect').value || '0', 10);
    var apSsidVal   = document.getElementById('apSsidInput').value || '';
    var apPassVal   = document.getElementById('apPassInput').value || '';
    var staSsidVal  = document.getElementById('staSsidInput').value || '';
    var staPassVal  = document.getElementById('staPassInput').value || '';

    var isoTempId       = document.getElementById('isoTempSensorSelect').value;
    var resinTempId     = document.getElementById('resinTempSensorSelect').value;
    var isoLowTempId    = document.getElementById('isoLowTempSensorSelect').value;
    var resinLowTempId  = document.getElementById('resinLowTempSensorSelect').value;
    var hose1TempId     = document.getElementById('hose1TempSensorSelect').value;
    var hose2TempId     = document.getElementById('hose2TempSensorSelect').value;

    var settingsPayload = {
      target: target,
      margin: margin,
      diff: diff,
      airTarget: airTargetVal,
      gunTarget: gunTargetVal,
      isoLowTarget: isoLowTargetVal,
      resinLowTarget: resinLowTargetVal,
      supplyLow: supplyLowVal,
      isoTempTarget: isoTempTargetVal,
      resinTempTarget: resinTempTargetVal,
      isoLowTempTarget: isoLowTempTargetVal,
      resinLowTempTarget: resinLowTempTargetVal,
      tempMinF: tempMinVal,
      tempMaxF: tempMaxVal,
      hose1Set: hose1SetVal,
      hose2Set: hose2SetVal,
      hose1Tol: hose1TolVal,
      hose2Tol: hose2TolVal,
      hoseOvertempF: hoseOvertempVal,
      wifiMode: wifiModeVal,
      apSsid: apSsidVal,
      apPass: apPassVal,
      staSsid: staSsidVal,
      staPass: staPassVal
    };

    var tempPayload = {
      iso:      isoTempId,
      resin:    resinTempId,
      isoLow:   isoLowTempId,
      resinLow: resinLowTempId,
      hose1:    hose1TempId,
      hose2:    hose2TempId
    };

    Promise.all([
      fetch('/api/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(settingsPayload)
      }),
      fetch('/api/temp-sensors', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(tempPayload)
      })
    ]).then(function(responses){
      if (!responses[0].ok || !responses[1].ok) {
        throw new Error('HTTP ' + responses[0].status + '/' + responses[1].status);
      }
      document.getElementById('saveStatus').textContent =
        'Settings saved. Network changes apply after reboot. Returning to live view…';
      setTimeout(function(){ window.location.href = '/'; }, 900);
    }).catch(function(err){
      document.getElementById('saveStatus').textContent = 'Save failed: ' + err.message;
    });
  });

  function setCalibStatus(msg){ document.getElementById('calibStatus').textContent = msg; }

  document.getElementById('zeroBtn').onclick = function(){
    var sensor = document.getElementById('sensorSelect').value;
    setCalibStatus('Zeroing ' + sensor + '…');
    fetch('/calibration', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sensor: sensor, action: 'zero' })
    }).then(function(r){ return r.json(); }).then(function(d){
      if(d.ok){
        setCalibStatus('Zero set for ' + sensor + ' (raw0=' + d.raw0.toFixed(2) + ').');
        refreshStatus();
      } else {
        setCalibStatus('Error: ' + (d.error || 'unknown'));
      }
    }).catch(function(e){ setCalibStatus('Zero failed: ' + e); });
  };

  // New: Zero all six sensors in one go
  document.getElementById('zeroAllBtn').onclick = function(){
    var order = ['iso','resin','isoLow','resinLow','air','apAir'];
    var index = 0;

    function zeroNext() {
      if (index >= order.length) {
        setCalibStatus('Zeroed all sensors.');
        refreshStatus();
        return;
      }
      var s = order[index++];
      setCalibStatus('Zeroing ' + s + ' (' + index + '/' + order.length + ')…');
      fetch('/calibration', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ sensor: s, action: 'zero' })
      }).then(function(r){ return r.json(); }).then(function(d){
        if (d.ok) {
          // proceed to the next sensor
          zeroNext();
        } else {
          setCalibStatus('Error on ' + s + ': ' + (d.error || 'unknown'));
        }
      }).catch(function(e){
        setCalibStatus('Zero failed on ' + s + ': ' + e);
      });
    }

    zeroNext();
  };

  document.getElementById('spanBtn').onclick = function(){
    var sensor = document.getElementById('sensorSelect').value;
    var p = parseFloat(document.getElementById('spanPressureInput').value);
    if(isNaN(p) || p <= 0){ setCalibStatus('Enter a valid known pressure first.'); return; }
    setCalibStatus('Setting span for ' + sensor + ' at ' + p + ' PSI…');
    fetch('/calibration', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sensor: sensor, action: 'span', pressure: p })
    }).then(function(r){ return r.json(); }).then(function(d){
      if(d.ok){
        setCalibStatus('Span set for ' + sensor + ' (K=' + d.K.toFixed(4) + ', raw1=' + d.raw1.toFixed(2) + ').');
        refreshStatus();
      } else {
        setCalibStatus('Error: ' + (d.error || 'unknown'));
      }
    }).catch(function(e){ setCalibStatus('Span failed: ' + e); });
  };

  function refreshStatus(){
    fetch('/calibration/status').then(function(r){ return r.json(); }).then(function(s){
      var tbody = document.getElementById('statusTable');
      tbody.innerHTML = '';
      var rows = [
        ['Iso (0–1600)','iso'],
        ['Resin (0–1600)','resin'],
        ['Iso Low (0–500)','isoLow'],
        ['Resin Low (0–500)','resinLow'],
        ['Air Piston (0–300)','air'],
        ['Gun AP Air (0–300)','apAir']
      ];
      rows.forEach(function(row){
        var label = row[0], key = row[1];
        var r0 = s[key].R0, K = s[key].K;
        var note;
        if(Math.abs(K-1.0) < 0.0001 && Math.abs(r0) < 0.0001){
          note = '<span class="warn">Not calibrated</span>';
        } else if (Math.abs(K-1.0) < 0.0001 && Math.abs(r0) >= 0.0001){
          note = '<span class="ok">Zeroed; span pending</span>';
        } else {
          note = '<span class="ok">Zero + Span set</span>';
        }
        tbody.insertAdjacentHTML('beforeend',
          '<tr><td>'+label+'</td><td>'+r0.toFixed(2)+'</td><td>'+K.toFixed(4)+'</td><td>'+note+'</td></tr>'
        );
      });
    });
  }

  refreshStatus();
</script>
</body>
</html>
)rawliteral";

// ---------- WebSocket events ----------

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.printf("WebSocket client #%u connected\n", num);
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("WebSocket client #%u disconnected\n", num);
  }
}

// Convert ADC reading from a 0.5–4.5 V pressure transducer to PSI based on its full-scale range
float readPSI_raw(int pin, float fullScalePsi) {
  const int samples = 8;
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  int adc = sum / samples;
  float vEsp = (adc / 4095.0f) * 3.3f;
  const float R_top = 10000.0f;
  const float R_bottom = 22000.0f;
  const float dividerRatio = R_bottom / (R_top + R_bottom);
  float vSensor = vEsp / dividerRatio;
  float psi = (vSensor - 0.5f) * (fullScalePsi / 4.0f);
  return psi;
}

// Apply zero/span calibration and clamp
float applyCalibration(float raw, float R0, float K, float fullScalePsi) {
  float p = (raw - R0) * K;
  if (p < 0) p = 0;
  if (p > fullScalePsi) p = fullScalePsi;
  return p;
}

// ---------- Hose heat control logic ----------
// "Swing" controller (asymmetric hysteresis):
//   - Heater turns ON when cooling down to the setpoint (temp <= set)
//   - Heater turns OFF once it reaches (set + swing)
// This minimizes cycling and matches slow hose thermal response.
// Disabled or missing sensor => relay OFF.

static inline void applyHoseHeatZone(bool enabled, float tempF, int setF, int swingF, bool& heating, int relayPin)
{
  if (!enabled) {
    heating = false;
    digitalWrite(relayPin, LOW);
    return;
  }
  if (isnan(tempF)) {
    heating = false;
    digitalWrite(relayPin, LOW);
    return;
  }

  // ON at setpoint (cooling down):
  // OFF at set + swing (warming up):
  const float onAt  = (float)setF;
  const float offAt = (float)setF + (float)swingF;

  if (!heating && tempF <= onAt) {
    heating = true;
  } else if (heating && tempF >= offAt) {
    heating = false;
  }

  digitalWrite(relayPin, heating ? HIGH : LOW);
}


static inline void tripHoseOvertemp(const char* zoneLabel, float tempF, int setF)
{
  if (!hoseOvertempActive) {
    hoseOvertempActive = true;
  }

  // Latch an interlock + park system + drop all hose heat outputs
  sprayInterlockActive = true;

  char buf[196];
  snprintf(buf, sizeof(buf),
           "Interlock: HOSE OVERTEMP - %s %.1f°F > %d°F + %d°F. Heaters OFF, system PARKED.",
           zoneLabel, tempF, setF, hoseOvertempF);
  lastInterlockReason = String(buf);

  // Park system and cut drum air (conservative safe state)
  sprayEnabled = false;
  drumAirEnabled = false;
  digitalWrite(RELAY_SPRAY_PIN, LOW);
  digitalWrite(RELAY_DRUM_AIR_PIN, LOW);

  // Drop hose relays immediately
  hose1Heating = false;
  hose2Heating = false;
  digitalWrite(RELAY_HOSE1_PIN, LOW);
  digitalWrite(RELAY_HOSE2_PIN, LOW);

  // Critical: also DISABLE hose heat so nothing can re-energize unexpectedly.
  // This is latched (persisted) so a reboot does not automatically restore heat.
  hose1Enabled = false;
  hose2Enabled = false;
  prefs.putBool("hose1En", false);
  prefs.putBool("hose2En", false);
}

static inline void applyHoseHeatControl()
{
  // Safety: if hose overtemp trip is active, force all hose heat relays OFF.
  if (hoseOvertempActive) {
    // Also ensure hose heat is disabled (UI shows OFF/Standby) and can't resume.
    if (hose1Enabled || hose2Enabled) {
      hose1Enabled = false;
      hose2Enabled = false;
      prefs.putBool("hose1En", false);
      prefs.putBool("hose2En", false);
    }
    hose1Heating = false;
    hose2Heating = false;
    digitalWrite(RELAY_HOSE1_PIN, LOW);
    digitalWrite(RELAY_HOSE2_PIN, LOW);
    return;
  }

  // Safety trip: if a hose exceeds (setpoint + hoseOvertempF), park and drop heaters.
  if (hose1Enabled && !isnan(hose1TempF) && hose1TempF >= (float)(hose1SetF + hoseOvertempF)) {
    tripHoseOvertemp("Hose 1", hose1TempF, hose1SetF);
    return;
  }
  if (hose2Enabled && !isnan(hose2TempF) && hose2TempF >= (float)(hose2SetF + hoseOvertempF)) {
    tripHoseOvertemp("Hose 2", hose2TempF, hose2SetF);
    return;
  }

  applyHoseHeatZone(hose1Enabled, hose1TempF, hose1SetF, hose1TolF, hose1Heating, RELAY_HOSE1_PIN);
  applyHoseHeatZone(hose2Enabled, hose2TempF, hose2SetF, hose2TolF, hose2Heating, RELAY_HOSE2_PIN);
}

static inline const char* hoseStatusText(bool enabled, bool heating, float tempF, int setF, int swingF)
{
  if (!enabled) return "OFF";
  if (isnan(tempF)) return "NO_SENSOR";
  if (heating) return "HEATING";
  // enabled, valid sensor, not heating
  (void)setF;
  (void)swingF;
  return "AT_TEMP";
}

// /api/settings
void handleSettings() {
  if (server.method() == HTTP_GET) {
    JsonDocument doc;
    doc["target"]         = targetPressure;
    doc["margin"]         = marginPercent;
    doc["diff"]           = diffPressure;
    doc["airTarget"]      = airTarget;
    doc["gunTarget"]      = gunTarget;
    doc["isoLowTarget"]   = isoLowTarget;
    doc["resinLowTarget"] = resinLowTarget;
    doc["supplyLow"]      = supplyLowPSI;

    doc["isoTempTarget"]      = isoTempTargetF;
    doc["resinTempTarget"]    = resinTempTargetF;
    doc["isoLowTempTarget"]   = isoLowTempTargetF;
    doc["resinLowTempTarget"] = resinLowTempTargetF;
    doc["tempMinF"]           = tempMinF;
    doc["tempMaxF"]           = tempMaxF;

    // Hose heat (2 zones)
    doc["hose1En"]  = hose1Enabled;
    doc["hose2En"]  = hose2Enabled;
    doc["hose1Set"] = hose1SetF;
    doc["hose2Set"] = hose2SetF;
    doc["hose1Tol"] = hose1TolF;
    doc["hose2Tol"] = hose2TolF;
    doc["hoseOvertempF"] = hoseOvertempF;
    doc["hoseOvertemp"] = hoseOvertempActive;

    doc["wifiMode"] = networkMode;
    doc["apSsid"]   = apSsid;
    doc["apPass"]   = apPass;
    doc["staSsid"]  = staSsid;
    doc["staPass"]  = staPass;

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  } else if (server.method() == HTTP_POST) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) {
      server.send(400, "text/plain", "Invalid JSON");
      return;
    }
    targetPressure      = doc["target"]         | targetPressure;
    marginPercent       = doc["margin"]         | marginPercent;
    diffPressure        = doc["diff"]           | diffPressure;
    airTarget           = doc["airTarget"]      | airTarget;
    gunTarget           = doc["gunTarget"]      | gunTarget;
    isoLowTarget        = doc["isoLowTarget"]   | isoLowTarget;
    resinLowTarget      = doc["resinLowTarget"] | resinLowTarget;
    supplyLowPSI        = doc["supplyLow"]      | supplyLowPSI;

    isoTempTargetF      = doc["isoTempTarget"]      | isoTempTargetF;
    resinTempTargetF    = doc["resinTempTarget"]    | resinTempTargetF;
    isoLowTempTargetF   = doc["isoLowTempTarget"]   | isoLowTempTargetF;
    resinLowTempTargetF = doc["resinLowTempTarget"] | resinLowTempTargetF;
    tempMinF            = doc["tempMinF"]           | tempMinF;
    tempMaxF            = doc["tempMaxF"]           | tempMaxF;

    // Hose heat settings
    hose1SetF = doc["hose1Set"] | hose1SetF;
    hose2SetF = doc["hose2Set"] | hose2SetF;
    hose1TolF = doc["hose1Tol"] | hose1TolF;
    hose2TolF = doc["hose2Tol"] | hose2TolF;
    if (doc["hose1En"].is<bool>()) hose1Enabled = doc["hose1En"];
    if (doc["hose2En"].is<bool>()) hose2Enabled = doc["hose2En"];

    if (doc["hoseOvertempF"].is<int>()) hoseOvertempF = doc["hoseOvertempF"];

    // Clamp sane ranges
    if (hose1SetF < 40) hose1SetF = 40;
    if (hose2SetF < 40) hose2SetF = 40;
    if (hose1SetF > 200) hose1SetF = 200;
    if (hose2SetF > 200) hose2SetF = 200;
    if (hose1TolF < 1) hose1TolF = 1;
    if (hose2TolF < 1) hose2TolF = 1;
    if (hose1TolF > 30) hose1TolF = 30;
    if (hose2TolF > 30) hose2TolF = 30;

    if (hoseOvertempF < 1) hoseOvertempF = 1;
    if (hoseOvertempF > 50) hoseOvertempF = 50;

    if (doc["wifiMode"].is<int>()) {
      networkMode = (int)doc["wifiMode"];
      if (networkMode != NETMODE_AP && networkMode != NETMODE_STA) {
        networkMode = NETMODE_AP;
      }
    }
    if (doc["apSsid"].is<const char*>()) {
      const char* v = doc["apSsid"];
      if (v) apSsid = String(v);
    }
    if (doc["apPass"].is<const char*>()) {
      const char* v = doc["apPass"];
      if (v) apPass = String(v);
    }
    if (doc["staSsid"].is<const char*>()) {
      const char* v = doc["staSsid"];
      if (v) staSsid = String(v);
    }
    if (doc["staPass"].is<const char*>()) {
      const char* v = doc["staPass"];
      if (v) staPass = String(v);
    }

    prefs.putInt("target",         targetPressure);
    prefs.putInt("margin",         marginPercent);
    prefs.putInt("diff",           diffPressure);
    prefs.putInt("airTarget",      airTarget);
    prefs.putInt("gunTarget",      gunTarget);
    prefs.putInt("isoLowTarget",   isoLowTarget);
    prefs.putInt("resinLowTarget", resinLowTarget);
    prefs.putInt("supplyLow",      supplyLowPSI);

    prefs.putInt("isoTempTarget",  isoTempTargetF);
    prefs.putInt("resTempTarget",  resinTempTargetF);
    prefs.putInt("isoLowTempTgt",  isoLowTempTargetF);
    prefs.putInt("resLowTempTgt",  resinLowTempTargetF);
    prefs.putInt("tempMinF",       tempMinF);
    prefs.putInt("tempMaxF",       tempMaxF);

    // Hose heat settings
    prefs.putBool("hose1En", hose1Enabled);
    prefs.putBool("hose2En", hose2Enabled);
    prefs.putInt("hose1SetF", hose1SetF);
    prefs.putInt("hose2SetF", hose2SetF);
    prefs.putInt("hose1TolF", hose1TolF);
    prefs.putInt("hose2TolF", hose2TolF);
    prefs.putInt("hoseOvertempF", hoseOvertempF);

    prefs.putInt("wifiMode", networkMode);
    prefs.putString("apSsid",  apSsid);
    prefs.putString("apPass",  apPass);
    prefs.putString("staSsid", staSsid);
    prefs.putString("staPass", staPass);

    server.send(200, "text/plain", "OK");
  } else {
    server.send(405, "text/plain", "Method Not Allowed");
  }
}

// /calibration
void handleCalibration() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  JsonDocument out;
  if (error) {
    out["ok"] = false;
    out["error"] = "Invalid JSON";
    String s;
    serializeJson(out, s);
    server.send(400, "application/json", s);
    return;
  }
  const char* sensor = doc["sensor"] | "";
  const char* action = doc["action"] | "";
  int   pin = -1;
  float fullScale = 0.0f;
  float* pR0 = nullptr;
  float* pK  = nullptr;
  if (!strcmp(sensor, "iso")) {
    pin = SENSOR_A_PIN;         fullScale = 1600.0f; pR0 = &isoR0;      pK = &isoK;
  } else if (!strcmp(sensor, "resin")) {
    pin = SENSOR_B_PIN;         fullScale = 1600.0f; pR0 = &resinR0;    pK = &resinK;
  } else if (!strcmp(sensor, "isoLow")) {
    pin = SENSOR_ISO_LOW_PIN;   fullScale = 500.0f;  pR0 = &isoLowR0;   pK = &isoLowK;
  } else if (!strcmp(sensor, "resinLow")) {
    pin = SENSOR_RESIN_LOW_PIN; fullScale = 500.0f;  pR0 = &resinLowR0; pK = &resinLowK;
  } else if (!strcmp(sensor, "air")) {
    pin = SENSOR_AIRPISTON_PIN; fullScale = 300.0f;  pR0 = &airR0;      pK = &airK;
  } else if (!strcmp(sensor, "apAir")) {
    pin = SENSOR_APAIR_PIN;     fullScale = 300.0f;  pR0 = &apAirR0;    pK = &apAirK;
  } else {
    out["ok"] = false;
    out["error"] = "Unknown sensor";
    String s;
    serializeJson(out, s);
    server.send(400, "application/json", s);
    return;
  }
  if (!strcmp(action, "zero")) {
    float raw0 = readPSI_raw(pin, fullScale);
    *pR0 = raw0;
    prefs.putFloat((String(sensor) + "_R0").c_str(), *pR0);
    prefs.putFloat((String(sensor) + "_K").c_str(), *pK);

  // Hose heat enable / setpoint / tolerance controls
  if (doc["hose1En"].is<bool>()) {
    hose1Enabled = doc["hose1En"];
    prefs.putBool("hose1En", hose1Enabled);
    if (!hose1Enabled) {
      hose1Heating = false;
      digitalWrite(RELAY_HOSE1_PIN, LOW);
    }
  }
  if (doc["hose2En"].is<bool>()) {
    hose2Enabled = doc["hose2En"];
    prefs.putBool("hose2En", hose2Enabled);
    if (!hose2Enabled) {
      hose2Heating = false;
      digitalWrite(RELAY_HOSE2_PIN, LOW);
    }
  }

  if (doc["hose1Set"].is<int>()) {
    hose1SetF = doc["hose1Set"];
    if (hose1SetF < 40) hose1SetF = 40;
    if (hose1SetF > 200) hose1SetF = 200;
    prefs.putInt("hose1SetF", hose1SetF);
  }
  if (doc["hose2Set"].is<int>()) {
    hose2SetF = doc["hose2Set"];
    if (hose2SetF < 40) hose2SetF = 40;
    if (hose2SetF > 200) hose2SetF = 200;
    prefs.putInt("hose2SetF", hose2SetF);
  }
  if (doc["hose1Tol"].is<int>()) {
    hose1TolF = doc["hose1Tol"];
    if (hose1TolF < 1) hose1TolF = 1;
    if (hose1TolF > 30) hose1TolF = 30;
    prefs.putInt("hose1TolF", hose1TolF);
  }
  if (doc["hose2Tol"].is<int>()) {
    hose2TolF = doc["hose2Tol"];
    if (hose2TolF < 1) hose2TolF = 1;
    if (hose2TolF > 30) hose2TolF = 30;
    prefs.putInt("hose2TolF", hose2TolF);

    if (doc["hoseOvertempF"].is<int>()) {
      hoseOvertempF = doc["hoseOvertempF"];
      if (hoseOvertempF < 1) hoseOvertempF = 1;
      if (hoseOvertempF > 50) hoseOvertempF = 50;
      prefs.putInt("hoseOvertempF", hoseOvertempF);
    }
  }

  // Re-evaluate hose heat immediately after any change
  applyHoseHeatControl();

  out["ok"]      = true;
    out["raw0"] = raw0;
  } else if (!strcmp(action, "span")) {
    if (!doc["pressure"].is<float>() && !doc["pressure"].is<int>()) {
      out["ok"] = false;
      out["error"] = "Missing pressure";
      String s;
      serializeJson(out, s);
      server.send(400, "application/json", s);
      return;
    }
    float pCal = doc["pressure"];
    if (pCal <= 0) {
      out["ok"] = false;
      out["error"] = "Pressure must be > 0";
      String s;
      serializeJson(out, s);
      server.send(400, "application/json", s);
      return;
    }
    float raw1 = readPSI_raw(pin, fullScale);
    float delta = raw1 - *pR0;
    if (fabs(delta) < 0.1f) {
      out["ok"] = false;
      out["error"] = "Delta too small, re-zero or use higher pressure";
      String s;
      serializeJson(out, s);
      server.send(400, "application/json", s);
      return;
    }
    *pK = pCal / delta;
    prefs.putFloat((String(sensor) + "_R0").c_str(), *pR0);
    prefs.putFloat((String(sensor) + "_K").c_str(),  *pK);
    out["ok"] = true;
    out["raw1"] = raw1;
    out["K"] = *pK;
  } else {
    out["ok"] = false;
    out["error"] = "Unknown action";
  }
  String s;
  serializeJson(out, s);
  server.send(200, "application/json", s);
}

// /calibration/status
void handleCalibrationStatus() {
  JsonDocument doc;
  JsonObject iso      = doc["iso"].to<JsonObject>();      iso["R0"] = isoR0;       iso["K"] = isoK;
  JsonObject resin    = doc["resin"].to<JsonObject>();    resin["R0"] = resinR0;   resin["K"] = resinK;
  JsonObject isoLow   = doc["isoLow"].to<JsonObject>();   isoLow["R0"] = isoLowR0; isoLow["K"] = isoLowK;
  JsonObject resinLow = doc["resinLow"].to<JsonObject>(); resinLow["R0"] = resinLowR0; resinLow["K"] = resinLowK;
  JsonObject air      = doc["air"].to<JsonObject>();      air["R0"] = airR0;       air["K"] = airK;
  JsonObject apAir    = doc["apAir"].to<JsonObject>();    apAir["R0"] = apAirR0;   apAir["K"] = apAirK;
  String s;
  serializeJson(doc, s);
  server.send(200, "application/json", s);
}

// /api/live - return the latest status JSON we broadcast to WebSocket/HMI
void handleLiveStatus() {
  if (lastStatusJson.length() == 0) {
    // Not ready yet; no status built
    server.send(503, "application/json", "{}");
    return;
  }
  server.send(200, "application/json", lastStatusJson);
}


// /api/temp-sensors
void handleTempSensors() {
  if (server.method() == HTTP_GET) {
    JsonDocument doc;
    JsonArray arr = doc["sensors"].to<JsonArray>();

    dsDeviceCount = tempSensors.getDeviceCount();
    DeviceAddress addr;

    // IMPORTANT: we do NOT call requestTemperatures() here.
    // We just read last-converted values to avoid blocking and lag.

    for (int i = 0; i < dsDeviceCount; i++) {
      if (tempSensors.getAddress(addr, i)) {
        String id = addressToString(addr);
        JsonObject sObj = arr.add<JsonObject>();
        sObj["id"] = id;

        float tC = tempSensors.getTempC(addr);
        if (tC > -100.0f) {
          float tF = tC * 9.0f / 5.0f + 32.0f;
          sObj["tempF"] = tF;
        }
      }
    }

    // Current assignments (exactly what is stored in NVS / memory)
    doc["iso"]      = isoTempAddrStr;      // HP Iso
    doc["resin"]    = resinTempAddrStr;    // HP Resin
    doc["isoLow"]   = isoLowTempAddrStr;   // Low-side Iso
    doc["resinLow"] = resinLowTempAddrStr; // Low-side Resin

    // Hose heat sensors
    doc["hose1"]   = hose1TempAddrStr;
    doc["hose2"]   = hose2TempAddrStr;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  }
  else if (server.method() == HTTP_POST) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
      server.send(400, "text/plain", "Invalid JSON");
      return;
    }

    const char* isoStr      = doc["iso"]      | "";
    const char* resinStr    = doc["resin"]    | "";
    const char* isoLowStr   = doc["isoLow"]   | "";
    const char* resinLowStr = doc["resinLow"] | "";
    const char* hose1Str   = doc["hose1"]   | "";
    const char* hose2Str   = doc["hose2"]   | "";

    isoTempAddrStr      = String(isoStr);
    resinTempAddrStr    = String(resinStr);
    isoLowTempAddrStr   = String(isoLowStr);
    resinLowTempAddrStr = String(resinLowStr);

    hose1TempAddrStr    = String(hose1Str);
    hose2TempAddrStr    = String(hose2Str);

    isoTempAssigned      = parseAddressString(isoTempAddrStr,      isoTempAddr);
    resinTempAssigned    = parseAddressString(resinTempAddrStr,    resinTempAddr);
    isoLowTempAssigned   = parseAddressString(isoLowTempAddrStr,   isoLowTempAddr);
    resinLowTempAssigned = parseAddressString(resinLowTempAddrStr, resinLowTempAddr);

    hose1TempAssigned = parseAddressString(hose1TempAddrStr, hose1TempAddr);
    hose2TempAssigned = parseAddressString(hose2TempAddrStr, hose2TempAddr);

  hose1TempAssigned = parseAddressString(hose1TempAddrStr, hose1TempAddr);
  hose2TempAssigned = parseAddressString(hose2TempAddrStr, hose2TempAddr);

    prefs.putString("isoTempAddr",    isoTempAddrStr);
    prefs.putString("resTempAddr",    resinTempAddrStr);
    prefs.putString("isoLowTempAddr", isoLowTempAddrStr);
    prefs.putString("resLowTempAddr", resinLowTempAddrStr);

    prefs.putString("hose1TempAddr", hose1TempAddrStr);
    prefs.putString("hose2TempAddr", hose2TempAddrStr);

    Serial.println("=== Temp assignments UPDATED via /api/temp-sensors ===");
    printDS18B20Addresses();

    JsonDocument outDoc;
    outDoc["ok"] = true;
    String out;
    serializeJson(outDoc, out);
    server.send(200, "application/json", out);
  }
  else {
    server.send(405, "text/plain", "Method Not Allowed");
  }
}

// /api/control  (for relays / modes)
void handleControl() {
  if (server.method() == HTTP_GET) {
    JsonDocument doc;
    doc["drumAir"] = drumAirEnabled;
    doc["spray"]   = sprayEnabled;
    doc["hose1En"] = hose1Enabled;
    doc["hose2En"] = hose2Enabled;
    doc["hose1Heat"] = hose1Heating;
    doc["hose2Heat"] = hose2Heating;
    doc["hose1Set"] = hose1SetF;
    doc["hose2Set"] = hose2SetF;
    doc["hose1Tol"] = hose1TolF;
    doc["hose2Tol"] = hose2TolF;
    doc["hoseOvertempF"] = hoseOvertempF;
    doc["hoseOvertemp"] = hoseOvertempActive;
    doc["hoseOvertempF"] = hoseOvertempF;
    doc["hoseOvertemp"] = hoseOvertempActive;
    if (!isnan(hose1TempF)) doc["hose1Temp"] = round1(hose1TempF);
    if (!isnan(hose2TempF)) doc["hose2Temp"] = round1(hose2TempF);
    doc["hose1Status"] = hoseStatusText(hose1Enabled, hose1Heating, hose1TempF, hose1SetF, hose1TolF);
    doc["hose2Status"] = hoseStatusText(hose2Enabled, hose2Heating, hose2TempF, hose2SetF, hose2TolF);
    String s;
    serializeJson(doc, s);
    server.send(200, "application/json", s);
    return;
  }

  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  JsonDocument out;
  if (err) {
    out["ok"] = false;
    out["error"] = "Invalid JSON";
    String s;
    serializeJson(out, s);
    server.send(400, "application/json", s);
    return;
  }

  if (doc["drumAir"].is<bool>()) {
    bool desired = doc["drumAir"];
    drumAirEnabled = desired;
    digitalWrite(RELAY_DRUM_AIR_PIN, drumAirEnabled ? HIGH : LOW);
    if (!drumAirEnabled) {
      // if you kill drum air, also drop spray as a safety
      sprayEnabled = false;
      digitalWrite(RELAY_SPRAY_PIN, LOW);
    }
  }

  if (doc["spray"].is<bool>()) {
    bool desired = doc["spray"];

    if (desired) {
      // Guard: block Spray if hose overtemp safety is active or hoses are above cutoff
      if (hoseOvertempActive ||
          (hose1Enabled && !isnan(hose1TempF) && hose1TempF >= (float)(hose1SetF + hoseOvertempF)) ||
          (hose2Enabled && !isnan(hose2TempF) && hose2TempF >= (float)(hose2SetF + hoseOvertempF))) {
        sprayInterlockActive = true;
        if (!hoseOvertempActive) {
          // set reason even if we haven't latched yet
          if (hose1Enabled && !isnan(hose1TempF) && hose1TempF >= (float)(hose1SetF + hoseOvertempF)) {
            lastInterlockReason = String("Interlock: HOSE OVERTEMP - Hose 1 above cutoff.");
          } else if (hose2Enabled && !isnan(hose2TempF) && hose2TempF >= (float)(hose2SetF + hoseOvertempF)) {
            lastInterlockReason = String("Interlock: HOSE OVERTEMP - Hose 2 above cutoff.");
          } else {
            lastInterlockReason = String("Interlock: HOSE OVERTEMP - safety trip active.");
          }
        }

        out["ok"] = false;
        out["error"] = "Hose overtemp safety";
        out["interlock"] = lastInterlockReason;
        String s;
        serializeJson(out, s);
        server.send(400, "application/json", s);
        return;
      }

      // Guard: only allow spray if both low sides above threshold and drum air is on
      if (!(lastIsoLowPSI >= supplyLowPSI && lastResinLowPSI >= supplyLowPSI)) {
        // Latch an interlock for low supply
        sprayInterlockActive = true;
        lastInterlockReason  = makeLowSupplyInterlockReason(lastIsoHPPSI, lastResinHPPSI,
                                                           lastIsoLowPSI, lastResinLowPSI,
                                                           supplyLowPSI);

        out["ok"]        = false;
        out["error"]     = "Low supply pressure";
        out["drumAir"] = drumAirEnabled;
  out["spray"]   = sprayEnabled;
  out["hose1En"] = hose1Enabled;
  out["hose2En"] = hose2Enabled;
  out["hose1Heat"] = hose1Heating;
  out["hose2Heat"] = hose2Heating;
  out["hose1Set"] = hose1SetF;
  out["hose2Set"] = hose2SetF;
  out["hose1Tol"] = hose1TolF;
  out["hose2Tol"] = hose2TolF;
  out["hose1Status"] = hoseStatusText(hose1Enabled, hose1Heating, hose1TempF, hose1SetF, hose1TolF);
  out["hose2Status"] = hoseStatusText(hose2Enabled, hose2Heating, hose2TempF, hose2SetF, hose2TolF);
        out["interlock"] = lastInterlockReason;
        String s;
        serializeJson(out, s);
        server.send(400, "application/json", s);
        return;
      }

      if (!drumAirEnabled) {
        // Latch an interlock for missing drum air
        sprayInterlockActive = true;
        lastInterlockReason  = "Interlock: drum air not enabled.";

        out["ok"]        = false;
        out["error"]     = "Drum air not enabled";
        out["drumAir"]   = drumAirEnabled;
        out["spray"]     = sprayEnabled;
        out["interlock"] = lastInterlockReason;
        String s;
        serializeJson(out, s);
        server.send(400, "application/json", s);
        return;
      }

      // Preconditions are good – enable spray and clear any latched interlock
      sprayEnabled = true;
      digitalWrite(RELAY_SPRAY_PIN, HIGH);
      sprayInterlockActive = false;
      lastInterlockReason  = "";
    } else {
      sprayEnabled = false;
      digitalWrite(RELAY_SPRAY_PIN, LOW);
    }
  }

  

// Hose heat enable / setpoint / swing controls (live page)
// Note: "Tol" on the UI is implemented as an OFF swing above setpoint:
//   ON  at temp <= setpoint
//   OFF at temp >= setpoint + swing
bool hoseTouched = false;

if (doc["hose1En"].is<bool>() || doc["hose1En"].is<int>()) {
  hose1Enabled = (doc["hose1En"].as<int>() != 0);
  prefs.putBool("hose1En", hose1Enabled);
  if (!hose1Enabled) {
    hose1Heating = false;
    digitalWrite(RELAY_HOSE1_PIN, LOW);
  }
  hoseTouched = true;
}

if (doc["hose2En"].is<bool>() || doc["hose2En"].is<int>()) {
  hose2Enabled = (doc["hose2En"].as<int>() != 0);
  prefs.putBool("hose2En", hose2Enabled);
  if (!hose2Enabled) {
    hose2Heating = false;
    digitalWrite(RELAY_HOSE2_PIN, LOW);
  }
  hoseTouched = true;
}

if (doc["hose1Set"].is<int>()) {
  hose1SetF = doc["hose1Set"].as<int>();
  hose1SetF = constrain(hose1SetF, 40, 200);
  prefs.putInt("hose1SetF", hose1SetF);
  hoseTouched = true;
}

if (doc["hose2Set"].is<int>()) {
  hose2SetF = doc["hose2Set"].as<int>();
  hose2SetF = constrain(hose2SetF, 40, 200);
  prefs.putInt("hose2SetF", hose2SetF);
  hoseTouched = true;
}

if (doc["hose1Tol"].is<int>()) {
  hose1TolF = doc["hose1Tol"].as<int>();
  hose1TolF = constrain(hose1TolF, 0, 20);
  prefs.putInt("hose1TolF", hose1TolF);
  hoseTouched = true;
}

if (doc["hose2Tol"].is<int>()) {
  hose2TolF = doc["hose2Tol"].as<int>();
  hose2TolF = constrain(hose2TolF, 0, 20);
  prefs.putInt("hose2TolF", hose2TolF);
  hoseTouched = true;
}

if (hoseTouched) {
  // Apply immediately so the UI doesn't appear to "flash" back on the next WS update
  applyHoseHeatControl();
}
out["ok"]      = true;
out["drumAir"] = drumAirEnabled;
out["spray"]   = sprayEnabled;

// Echo full hose heat state so the web UI can update immediately
out["hose1En"]   = hose1Enabled;
out["hose2En"]   = hose2Enabled;
out["hose1Heat"] = hose1Heating;
out["hose2Heat"] = hose2Heating;
out["hose1Set"]  = hose1SetF;
out["hose2Set"]  = hose2SetF;
out["hose1Tol"]  = hose1TolF;
out["hose2Tol"]  = hose2TolF;
if (!isnan(hose1TempF)) out["hose1Temp"] = round1(hose1TempF);
if (!isnan(hose2TempF)) out["hose2Temp"] = round1(hose2TempF);
out["hose1Status"] = hoseStatusText(hose1Enabled, hose1Heating, hose1TempF, hose1SetF, hose1TolF);
out["hose2Status"] = hoseStatusText(hose2Enabled, hose2Heating, hose2TempF, hose2SetF, hose2TolF);
String s;
  serializeJson(out, s);
  server.send(200, "application/json", s);
}

// ---------- Interlock reset (web UI) ----------
static bool hoseOvertempConditionCleared()
{
  // Require at least one assigned hose sensor to evaluate.
  bool anyAssigned = false;

  if (hose1TempAssigned) {
    anyAssigned = true;
    if (isnan(hose1TempF)) return false;
    if (hose1TempF > (float)(hose1SetF + hoseOvertempF)) return false;
  }
  if (hose2TempAssigned) {
    anyAssigned = true;
    if (isnan(hose2TempF)) return false;
    if (hose2TempF > (float)(hose2SetF + hoseOvertempF)) return false;
  }

  return anyAssigned;
}

static bool lowSupplyConditionCleared()
{
  return (lastIsoLowPSI >= supplyLowPSI) && (lastResinLowPSI >= supplyLowPSI);
}

static void handleInterlockReset()
{
  JsonDocument out;

  const bool anyInterlock = sprayInterlockActive || hoseOvertempActive;
  if (!anyInterlock) {
    out["ok"] = true;
    out["message"] = "No active interlock.";
    out["interlock"] = "";
    String s; serializeJson(out, s);
    server.send(200, "application/json", s);
    return;
  }

  bool canClear = true;
  String denyReason;

  // For hose overtemp, require temps back under threshold.
  if (hoseOvertempActive || lastInterlockReason.indexOf("HOSE OVERTEMP") >= 0) {
    canClear = hoseOvertempConditionCleared();
    if (!canClear) denyReason = "Hose overtemp condition not cleared.";
  }
  // For low supply interlock, require both low-side feeds above threshold.
  else if (lastInterlockReason.indexOf("low supply pressure") >= 0) {
    canClear = lowSupplyConditionCleared();
    if (!canClear) denyReason = "Low supply pressure condition not cleared.";
  }
  // For drum air interlock, require drum air enabled.
  else if (lastInterlockReason.indexOf("drum air not enabled") >= 0) {
    canClear = drumAirEnabled;
    if (!canClear) denyReason = "Drum air not enabled.";
  }

  if (!canClear) {
    out["ok"] = false;
    out["error"] = denyReason;
    out["interlock"] = lastInterlockReason;
    String s; serializeJson(out, s);
    server.send(409, "application/json", s);
    return;
  }

  // Clear interlocks and put system into a safe READY state (parked, outputs off).
  sprayInterlockActive = false;
  hoseOvertempActive   = false;
  lastInterlockReason  = "";

  sprayEnabled   = false;
  drumAirEnabled = false;
  digitalWrite(RELAY_SPRAY_PIN, LOW);
  digitalWrite(RELAY_DRUM_AIR_PIN, LOW);

  hose1Heating = false;
  hose2Heating = false;
  digitalWrite(RELAY_HOSE1_PIN, LOW);
  digitalWrite(RELAY_HOSE2_PIN, LOW);

  out["ok"] = true;
  out["message"] = "Interlock cleared. System READY (parked).";
  out["interlock"] = "";
  String s; serializeJson(out, s);
  server.send(200, "application/json", s);
}



// OTA upload handler
void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  static bool updateError = false;

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA: Update start: %s\n", upload.filename.c_str());
    updateError = false;
    if (!Update.begin()) {
      Update.printError(Serial);
      updateError = true;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!updateError) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
        updateError = true;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!updateError) {
      if (Update.end(true)) { // true to set the size to the current progress
        Serial.printf("OTA: Update success, %u bytes. Rebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
        updateError = true;
      }
    } else {
      Serial.println("OTA: Update failed during write");
    }
  }
}

// Serve main page with version injected
void handleRoot() {
  String html = mainPage;
  html.replace("{{FW_VERSION}}", FW_VERSION);
  server.send(200, "text/html", html);
}

// Serve settings page with version injected
void handleSettingsPage() {
  String html = settingsPage;
  html.replace("{{FW_VERSION}}", FW_VERSION);
  server.send(200, "text/html", html);
}

// Serve update page with version injected
void handleUpdatePage() {
  String html = updatePage;
  html.replace("{{FW_VERSION}}", FW_VERSION);
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Foam rig boot ===");

  // HMI UART on GPIO16 (RX) and GPIO17 (TX)
  HMISerial.begin(115200, SERIAL_8N1, 16, 17);
  Serial.println("HMI UART started on GPIO16/17 @115200");


  // Relay outputs
  pinMode(RELAY_SPRAY_PIN, OUTPUT);
  pinMode(RELAY_DRUM_AIR_PIN, OUTPUT);
  pinMode(RELAY_HOSE1_PIN, OUTPUT);
  pinMode(RELAY_HOSE2_PIN, OUTPUT);
  digitalWrite(RELAY_SPRAY_PIN, LOW);
  digitalWrite(RELAY_DRUM_AIR_PIN, LOW);
  digitalWrite(RELAY_HOSE1_PIN, LOW);
  digitalWrite(RELAY_HOSE2_PIN, LOW);


  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_A_PIN,         ADC_11db);
  analogSetPinAttenuation(SENSOR_B_PIN,         ADC_11db);
  analogSetPinAttenuation(SENSOR_AIRPISTON_PIN, ADC_11db);
  analogSetPinAttenuation(SENSOR_APAIR_PIN,     ADC_11db);
  analogSetPinAttenuation(SENSOR_ISO_LOW_PIN,    ADC_11db);
  analogSetPinAttenuation(SENSOR_RESIN_LOW_PIN,  ADC_11db);
  // Hose-tip LED (PWM)
  hoseLedInit();
  tempSensors.begin();

  prefs.begin("foam", false);
  targetPressure = prefs.getInt("target", 1000);
  marginPercent  = prefs.getInt("margin", 10);
  diffPressure   = prefs.getInt("diff", 50);
  airTarget      = prefs.getInt("airTarget", 100);
  gunTarget      = prefs.getInt("gunTarget", 100);
  isoLowTarget   = prefs.getInt("isoLowTarget",   200);
  resinLowTarget = prefs.getInt("resinLowTarget", 200);
  supplyLowPSI   = prefs.getInt("supplyLow",      150);

  isoTempTargetF      = prefs.getInt("isoTempTarget",   120);
  resinTempTargetF    = prefs.getInt("resTempTarget",   120);
  isoLowTempTargetF   = prefs.getInt("isoLowTempTgt",   isoTempTargetF);
  resinLowTempTargetF = prefs.getInt("resLowTempTgt",   resinTempTargetF);
  tempMinF            = prefs.getInt("tempMinF",        40);
  tempMaxF            = prefs.getInt("tempMaxF",        180);

  // Hose heat (2 zones)
  hose1Enabled = prefs.getBool("hose1En", false);
  hose2Enabled = prefs.getBool("hose2En", false);
  hose1SetF    = prefs.getInt("hose1SetF", 125);
  hose2SetF    = prefs.getInt("hose2SetF", 125);
  hose1TolF    = prefs.getInt("hose1TolF", 3);
  hose2TolF    = prefs.getInt("hose2TolF", 3);
  hoseOvertempF = prefs.getInt("hoseOvertempF", 5);
  if (hoseOvertempF < 1) hoseOvertempF = 1;
  if (hoseOvertempF > 50) hoseOvertempF = 50;

  // Safety: never energize hose heaters automatically on boot.
  // After any reboot/power-cycle, the user must explicitly re-enable hose heat.
  if (hose1Enabled || hose2Enabled) {
    hose1Enabled = false;
    hose2Enabled = false;
    prefs.putBool("hose1En", false);
    prefs.putBool("hose2En", false);
  }


  isoR0   = prefs.getFloat("iso_R0",   0.0f);
  isoK    = prefs.getFloat("iso_K",    1.0f);
  resinR0 = prefs.getFloat("resin_R0", 0.0f);
  resinK  = prefs.getFloat("resin_K",  1.0f);
  isoLowR0   = prefs.getFloat("isoLow_R0",   0.0f);
  isoLowK    = prefs.getFloat("isoLow_K",    1.0f);
  resinLowR0 = prefs.getFloat("resinLow_R0", 0.0f);
  resinLowK  = prefs.getFloat("resinLow_K",  1.0f);
  airR0   = prefs.getFloat("air_R0",   0.0f);
  airK    = prefs.getFloat("air_K",    1.0f);
  apAirR0 = prefs.getFloat("apAir_R0", 0.0f);
  apAirK  = prefs.getFloat("apAir_K",  1.0f);

  isoTempAddrStr      = prefs.getString("isoTempAddr",    "");
  resinTempAddrStr    = prefs.getString("resTempAddr",    "");
  isoLowTempAddrStr   = prefs.getString("isoLowTempAddr", "");
  resinLowTempAddrStr = prefs.getString("resLowTempAddr", "");

  hose1TempAddrStr    = prefs.getString("hose1TempAddr", "");
  hose2TempAddrStr    = prefs.getString("hose2TempAddr", "");

  isoTempAssigned      = parseAddressString(isoTempAddrStr,      isoTempAddr);
  resinTempAssigned    = parseAddressString(resinTempAddrStr,    resinTempAddr);
  isoLowTempAssigned   = parseAddressString(isoLowTempAddrStr,   isoLowTempAddr);
  resinLowTempAssigned = parseAddressString(resinLowTempAddrStr, resinLowTempAddr);

  // Hose heat temp sensor assignments (must be parsed at boot so live temps render)
  hose1TempAssigned    = parseAddressString(hose1TempAddrStr, hose1TempAddr);
  hose2TempAssigned    = parseAddressString(hose2TempAddrStr, hose2TempAddr);

  networkMode = prefs.getInt("wifiMode", (int)NETMODE_AP);
  apSsid      = prefs.getString("apSsid",  DEFAULT_AP_SSID);
  apPass      = prefs.getString("apPass",  DEFAULT_AP_PASS);
  staSsid     = prefs.getString("staSsid", "");
  staPass     = prefs.getString("staPass", "");

  printDS18B20Addresses();

  setupWiFi();

  if (MDNS.begin("foam")) {
    Serial.println("mDNS: foam.local");
  } else {
    Serial.println("mDNS failed");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_GET, handleSettingsPage);
  server.on("/api/settings", handleSettings);
  server.on("/calibration", handleCalibration);
  server.on("/calibration/status", HTTP_GET, handleCalibrationStatus);
  server.on("/api/temp-sensors", handleTempSensors);
  server.on("/api/control", handleControl);
  server.on("/api/interlock/reset", HTTP_POST, handleInterlockReset);
  server.on("/api/live", HTTP_GET, handleLiveStatus);

  // OTA endpoints
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on(
    "/update", HTTP_POST,
    []() {
      // Called when upload is finished
      if (Update.hasError()) {
        const char* failPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>Firmware Update Failed</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <style>
    body {
      margin:0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background:#020617;
      color:#e5e7eb;
      display:flex;
      align-items:center;
      justify-content:center;
      min-height:100vh;
    }
    .card {
      padding:1.25rem 1.5rem;
      border-radius:16px;
      border:1px solid #b91c1c;
      background:#111827;
      max-width:420px;
    }
    h1 { margin-top:0; font-size:1.1rem; color:#fecaca; }
    p { font-size:0.85rem; color:#e5e7eb; }
    a { color:#38bdf8; text-decoration:none; font-size:0.85rem; }
  </style>
</head>
<body>
  <div class="card">
    <h1>Firmware Update Failed</h1>
    <p>There was a problem writing the new firmware. Please verify the .bin file and try again.</p>
    <p><a href="/update">Back to update page</a></p>
  </div>
</body>
</html>
)rawliteral";
        server.send(500, "text/html", failPage);
      } else {
        const char* okPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>Firmware Update OK</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <!-- Redirect to live page after 10 seconds -->
  <meta http-equiv="refresh" content="10;url=/" />
  <style>
    body {
      margin:0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background:#020617;
      color:#e5e7eb;
      display:flex;
      align-items:center;
      justify-content:center;
      min-height:100vh;
    }
    .card {
      padding:1.25rem 1.5rem;
      border-radius:16px;
      border:1px solid #1e293b;
      background:#020617;
      max-width:420px;
      text-align:center;
    }
    h1 { margin-top:0; font-size:1.1rem; }
    p { font-size:0.85rem; color:#9ca3af; }
    .countdown { font-weight:600; color:#38bdf8; }
  </style>
  <script>
    // JS backup redirect + simple countdown
    let remaining = 10;
    function tick() {
      var el = document.getElementById('countdown');
      if (el) el.textContent = remaining;
      if (remaining <= 0) {
        window.location.href = '/';
      } else {
        remaining--;
        setTimeout(tick, 1000);
      }
    }
    window.addEventListener('DOMContentLoaded', tick);
  </script>
</head>
<body>
  <div class="card">
    <h1>Firmware Update Successful</h1>
    <p>The rig is rebooting into the new firmware.</p>
    <p>Returning to the live dashboard in <span id="countdown" class="countdown">10</span> seconds…</p>
    <p>If it doesn’t redirect automatically, you can <a href="/">tap here to go to the main page</a>.</p>
  </div>
</body>
</html>
)rawliteral";
        server.send(200, "text/html", okPage);
        // Give the response a moment to flush before reboot
        delay(500);
        ESP.restart();
      }
    },
    handleUpdateUpload
  );

  server.begin();
  Serial.println("HTTP server started");

  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  Serial.println("WebSocket server started");
}


// -----------------------------------------------------------------------------
// HMI UART polling and button handling (no JSON dependency)
// -----------------------------------------------------------------------------

// Read bytes from HMISerial, assemble into newline-terminated lines
void hmiPollUart()
{
  while (HMISerial.available() > 0) {
    char c = (char)HMISerial.read();

    // Ignore CR, treat LF as line terminator
    if (c == '\r') continue;

    if (c == '\n') {
      if (hmiRxBuffer.length() == 0) {
        // Empty line, ignore
        continue;
      }

      // Debug: show exactly what we received
      Serial.print("RX line: [");
      Serial.print(hmiRxBuffer);
      Serial.println("]");

      // Parse JSON commands from the HMI (ArduinoJson)
      char first = hmiRxBuffer[0];
      if (first == '{') {
        // Newline-delimited JSON control messages from the HMI.
        // This intentionally mirrors the Live page controls (/api/control), but without exposing Settings.
        JsonDocument cmdDoc;
        DeserializationError jerr = deserializeJson(cmdDoc, hmiRxBuffer);

        if (!jerr) {
          bool hoseTouched = false;

          // ---- Optional request: state snapshot ----
          // Accept {"cmd":"request_state"} or {"cmd":"state"} (also keeps legacy {"cmd":"..."} behavior below).
          const char* cmd = cmdDoc["cmd"] | "";
          if (cmd[0] != '\0') {
            if (!strcmp(cmd, "request_state") || !strcmp(cmd, "state") || !strcmp(cmd, "live")) {
              if (lastStatusJson.length()) {
                HMISerial.write((const uint8_t*)lastStatusJson.c_str(), lastStatusJson.length());
                HMISerial.write('\n');
              }
            }

            // Legacy toggles: {"cmd":"drum"} / {"cmd":"spray"}
            if (!strcmp(cmd, "drum")) {
              bool desired = !drumAirEnabled; // toggle
              drumAirEnabled = desired;
              digitalWrite(RELAY_DRUM_AIR_PIN, drumAirEnabled ? HIGH : LOW);

              // If you kill drum air, also drop spray as a safety
              if (!drumAirEnabled) {
                sprayEnabled = false;
                digitalWrite(RELAY_SPRAY_PIN, LOW);
              }
            }
            else if (!strcmp(cmd, "spray")) {
              bool desired = !sprayEnabled; // toggle via legacy command
              cmdDoc["spray"] = desired;    // normalize to set-style below
            }
          }

          // ---- Set-style controls (preferred) ----
          // Relays
          if (cmdDoc["drumAir"].is<bool>() || cmdDoc["drumAir"].is<int>()) {
            bool desired = (cmdDoc["drumAir"].as<int>() != 0);
            drumAirEnabled = desired;
            digitalWrite(RELAY_DRUM_AIR_PIN, drumAirEnabled ? HIGH : LOW);

            if (!drumAirEnabled) {
              sprayEnabled = false;
              digitalWrite(RELAY_SPRAY_PIN, LOW);
            }
          }

          // Hose enable
          if (cmdDoc["hose1En"].is<bool>() || cmdDoc["hose1En"].is<int>()) {
            hose1Enabled = (cmdDoc["hose1En"].as<int>() != 0);
            prefs.putBool("hose1En", hose1Enabled);
            if (!hose1Enabled) {
              hose1Heating = false;
              digitalWrite(RELAY_HOSE1_PIN, LOW);
            }
            hoseTouched = true;
          }

          if (cmdDoc["hose2En"].is<bool>() || cmdDoc["hose2En"].is<int>()) {
            hose2Enabled = (cmdDoc["hose2En"].as<int>() != 0);
            prefs.putBool("hose2En", hose2Enabled);
            if (!hose2Enabled) {
              hose2Heating = false;
              digitalWrite(RELAY_HOSE2_PIN, LOW);
            }
            hoseTouched = true;
          }

          // Hose setpoints / swing ("Tol" on UI behaves as OFF swing above setpoint)
          if (cmdDoc["hose1Set"].is<int>()) {
            hose1SetF = constrain(cmdDoc["hose1Set"].as<int>(), 40, 200);
            prefs.putInt("hose1SetF", hose1SetF);
            hoseTouched = true;
          }

          if (cmdDoc["hose2Set"].is<int>()) {
            hose2SetF = constrain(cmdDoc["hose2Set"].as<int>(), 40, 200);
            prefs.putInt("hose2SetF", hose2SetF);
            hoseTouched = true;
          }

          if (cmdDoc["hose1Tol"].is<int>()) {
            hose1TolF = constrain(cmdDoc["hose1Tol"].as<int>(), 0, 50);
            prefs.putInt("hose1TolF", hose1TolF);
            hoseTouched = true;
          }

          if (cmdDoc["hose2Tol"].is<int>()) {
            hose2TolF = constrain(cmdDoc["hose2Tol"].as<int>(), 0, 50);
            prefs.putInt("hose2TolF", hose2TolF);
            hoseTouched = true;
          }

          if (cmdDoc["hoseOvertempF"].is<int>()) {
            hoseOvertempF = constrain(cmdDoc["hoseOvertempF"].as<int>(), 0, 50);
            prefs.putInt("hoseOvertempF", hoseOvertempF);
            hoseTouched = true;
          }

          if (hoseTouched) {
            applyHoseHeatControl();
          }

          // Spray control: same guards as web UI
          if (cmdDoc["spray"].is<bool>() || cmdDoc["spray"].is<int>()) {
            bool desired = (cmdDoc["spray"].as<int>() != 0);

            if (desired) {
              // Block if hose overtemp is active or conditions exceed cutoff
              if (hoseOvertempActive || !hoseOvertempConditionCleared()) {
                sprayInterlockActive = true;
                // Preserve the canonical HOSE OVERTEMP prefix for reset matching
                if (hose1Enabled && !isnan(hose1TempF) && hose1TempF >= (float)(hose1SetF + hoseOvertempF)) {
                  lastInterlockReason = String("Interlock: HOSE OVERTEMP - Hose 1 above cutoff.");
                } else if (hose2Enabled && !isnan(hose2TempF) && hose2TempF >= (float)(hose2SetF + hoseOvertempF)) {
                  lastInterlockReason = String("Interlock: HOSE OVERTEMP - Hose 2 above cutoff.");
                } else {
                  lastInterlockReason = String("Interlock: HOSE OVERTEMP - safety trip active.");
                }

                sprayEnabled = false;
                digitalWrite(RELAY_SPRAY_PIN, LOW);
              }
              // Low-side supply guard
              else if (!(lastIsoLowPSI >= supplyLowPSI && lastResinLowPSI >= supplyLowPSI)) {
                sprayInterlockActive = true;
                lastInterlockReason  = makeLowSupplyInterlockReason(lastIsoHPPSI, lastResinHPPSI,
                                                                   lastIsoLowPSI, lastResinLowPSI,
                                                                   supplyLowPSI);
                sprayEnabled = false;
                digitalWrite(RELAY_SPRAY_PIN, LOW);
              }
              // Drum air must be enabled to spray
              else if (!drumAirEnabled) {
                sprayInterlockActive = true;
                lastInterlockReason  = String("Interlock: drum air not enabled.");
                sprayEnabled = false;
                digitalWrite(RELAY_SPRAY_PIN, LOW);
              }
              else {
                // Preconditions OK – enable spray and clear spray interlock
                sprayEnabled         = true;
                digitalWrite(RELAY_SPRAY_PIN, HIGH);
                sprayInterlockActive = false;
                if (lastInterlockReason.indexOf("low supply pressure") >= 0 ||
                    lastInterlockReason.indexOf("drum air not enabled") >= 0 ||
                    lastInterlockReason.indexOf("HOSE OVERTEMP") >= 0) {
                  lastInterlockReason = "";
                }
              }
            } else {
              sprayEnabled = false;
              digitalWrite(RELAY_SPRAY_PIN, LOW);
            }
          }

          // Interlock reset from HMI: {"resetInterlock":true} or {"interlockReset":true}
          if ((cmdDoc["resetInterlock"].is<bool>() && (bool)cmdDoc["resetInterlock"]) ||
              (cmdDoc["interlockReset"].is<bool>() && (bool)cmdDoc["interlockReset"]) ||
              (cmdDoc["resetInterlock"].is<int>() && cmdDoc["resetInterlock"].as<int>() != 0) ||
              (cmdDoc["interlockReset"].is<int>() && cmdDoc["interlockReset"].as<int>() != 0)) {

            // Mirror the web reset behavior: only clear if triggering condition is cleared.
            bool canClear = true;
            String denyReason;

            if (hoseOvertempActive || lastInterlockReason.indexOf("HOSE OVERTEMP") >= 0) {
              canClear = hoseOvertempConditionCleared();
              if (!canClear) denyReason = "Hose overtemp condition not cleared.";
            } else if (lastInterlockReason.indexOf("low supply pressure") >= 0) {
              canClear = lowSupplyConditionCleared();
              if (!canClear) denyReason = "Low supply pressure condition not cleared.";
            } else if (lastInterlockReason.indexOf("drum air not enabled") >= 0) {
              canClear = drumAirEnabled;
              if (!canClear) denyReason = "Drum air not enabled.";
            }

            if (canClear) {
              sprayInterlockActive = false;
              hoseOvertempActive   = false;
              lastInterlockReason  = "";

              sprayEnabled   = false;
              drumAirEnabled = false;
              digitalWrite(RELAY_SPRAY_PIN, LOW);
              digitalWrite(RELAY_DRUM_AIR_PIN, LOW);

              hose1Heating = false;
              hose2Heating = false;
              digitalWrite(RELAY_HOSE1_PIN, LOW);
              digitalWrite(RELAY_HOSE2_PIN, LOW);

              applyHoseHeatControl();
            } else {
              // Keep interlock latched; just log deny reason for debugging.
              Serial.print("HMI reset denied: ");
              Serial.println(denyReason);
            }
          }
        } else {
          Serial.print("HMI JSON parse error: ");
          Serial.println(jerr.c_str());
        }
      }// Reset buffer for next line
      hmiRxBuffer = "";
    } else {
      // Regular character – append as long as we don't overflow
      if (hmiRxBuffer.length() < 255) {
        hmiRxBuffer += c;
      } else {
        // Overflow safeguard – drop the line
        hmiRxBuffer = "";
      }
    }
  }
}

void loop() {
  server.handleClient();
  webSocket.loop();
  handleWifiFallback();

  // Poll UART link from HMI for control commands
  hmiPollUart();


  static unsigned long last         = 0;
  static unsigned long lastTempRead = 0;

  unsigned long now = millis();

  if (now - last > 200) {
    float rawIso      = readPSI_raw(SENSOR_A_PIN,         1600.0f);
    float rawResin    = readPSI_raw(SENSOR_B_PIN,         1600.0f);
    float rawIsoLow   = readPSI_raw(SENSOR_ISO_LOW_PIN,    500.0f);
    float rawResinLow = readPSI_raw(SENSOR_RESIN_LOW_PIN,  500.0f);
    float rawAir      = readPSI_raw(SENSOR_AIRPISTON_PIN,  300.0f);
    float rawApAir    = readPSI_raw(SENSOR_APAIR_PIN,      300.0f);

    float isoPSI       = applyCalibration(rawIso,      isoR0,      isoK,      1600.0f);
    float resinPSI     = applyCalibration(rawResin,    resinR0,    resinK,    1600.0f);
    float isoLowPSI    = applyCalibration(rawIsoLow,   isoLowR0,   isoLowK,    500.0f);
    float resinLowPSI  = applyCalibration(rawResinLow, resinLowR0, resinLowK,  500.0f);
    float airPistonPSI = applyCalibration(rawAir,      airR0,      airK,       300.0f);
    float apAirPSI     = applyCalibration(rawApAir,    apAirR0,    apAirK,     300.0f);

    // Cache latest readings for UI banners / interlock snapshot text.
    lastIsoHPPSI   = isoPSI;
    lastResinHPPSI = resinPSI;
    lastIsoLowPSI   = isoLowPSI;
    lastResinLowPSI = resinLowPSI;

    // Auto-interlock: if in Spray mode and either low side drops below threshold,
    // park (spray off) and shut off drum pump air.
    if (sprayEnabled && (isoLowPSI < supplyLowPSI || resinLowPSI < supplyLowPSI)) {
      Serial.println("Supply low: auto park & drum air off");
      sprayEnabled   = false;
      drumAirEnabled = false;
      digitalWrite(RELAY_SPRAY_PIN, LOW);
      digitalWrite(RELAY_DRUM_AIR_PIN, LOW);
      sprayInterlockActive = true;
      lastInterlockReason  = makeLowSupplyInterlockReason(isoPSI, resinPSI,
                                                         isoLowPSI, resinLowPSI,
                                                         supplyLowPSI);
    }

    // --- Hose-tip LED mode based on Iso HP vs green band ---
    float bandFrac = marginPercent / 100.0f;
    if (bandFrac < 0.0f) bandFrac = 0.0f;
    if (bandFrac > 1.0f) bandFrac = 1.0f;

    float isoBandLow  = targetPressure * (1.0f - bandFrac);
    float isoBandHigh = targetPressure * (1.0f + bandFrac);

    if (isoPSI <= 0.0f) {
      // Rig idle
      hoseLedMode = HOSE_LED_PULSE;
    } else if (isoPSI < isoBandLow) {
      // Below green band → slow blink
      hoseLedMode = HOSE_LED_BLINK_SLOW;
    } else if (isoPSI > isoBandHigh) {
      // Above green band / setpoint → fast blink
      hoseLedMode = HOSE_LED_BLINK_FAST;
    } else {
      // Within green band → solid
      hoseLedMode = HOSE_LED_SOLID;
    }

    // Update hose heat relays/state before we publish status
    applyHoseHeatControl();

    // Build status JSON for WebSocket + HMI UART (ArduinoJson)
    JsonDocument doc;

    // Pressures
    doc["iso"]      = round1(isoPSI);
    doc["resin"]    = round1(resinPSI);
    doc["isoLow"]   = round1(isoLowPSI);
    doc["resinLow"] = round1(resinLowPSI);
    doc["airPiston"]= round1(airPistonPSI);
    doc["apAir"]    = round1(apAirPSI);

    // Temps (only include when valid)
    if (!isnan(isoTempF))       doc["isoTemp"]      = round1(isoTempF);
    if (!isnan(resinTempF))     doc["resinTemp"]    = round1(resinTempF);
    if (!isnan(isoLowTempF))    doc["isoLowTemp"]   = round1(isoLowTempF);
    if (!isnan(resinLowTempF))  doc["resinLowTemp"] = round1(resinLowTempF);

    // Relay / mode states (0/1 so JS + LVGL can treat them as booleans)
    doc["drumAir"] = drumAirEnabled ? 1 : 0;
    doc["spray"]   = sprayEnabled   ? 1 : 0;

    // Hose heat (2 zones)
    doc["hose1En"]   = hose1Enabled ? 1 : 0;
    doc["hose2En"]   = hose2Enabled ? 1 : 0;
    doc["hose1Heat"] = hose1Heating ? 1 : 0;
    doc["hose2Heat"] = hose2Heating ? 1 : 0;
    doc["hose1Set"]  = hose1SetF;
    doc["hose2Set"]  = hose2SetF;
    doc["hose1Tol"]  = hose1TolF;
    doc["hose2Tol"]  = hose2TolF;
    doc["hoseOvertempF"] = hoseOvertempF;
    doc["hoseOvertemp"]  = hoseOvertempActive ? 1 : 0;
    if (!isnan(hose1TempF)) doc["hose1Temp"] = round1(hose1TempF);
    if (!isnan(hose2TempF)) doc["hose2Temp"] = round1(hose2TempF);
    doc["hose1Status"] = hoseStatusText(hose1Enabled, hose1Heating, hose1TempF, hose1SetF, hose1TolF);
    doc["hose2Status"] = hoseStatusText(hose2Enabled, hose2Heating, hose2TempF, hose2SetF, hose2TolF);

    // Firmware and configuration (mirrors the web UI expectations)
    doc["fw"]             = FW_VERSION;
    doc["target"]         = targetPressure;
    doc["margin"]         = marginPercent;
    doc["diff"]           = diffPressure;
    doc["airTarget"]      = airTarget;
    doc["gunTarget"]      = gunTarget;
    doc["isoLowTarget"]   = isoLowTarget;
    doc["resinLowTarget"] = resinLowTarget;
    doc["supplyLow"]      = supplyLowPSI;

    doc["isoTempTarget"]      = isoTempTargetF;
    doc["resinTempTarget"]    = resinTempTargetF;
    doc["isoLowTempTarget"]   = isoLowTempTargetF;
    doc["resinLowTempTarget"] = resinLowTempTargetF;
    doc["tempMinF"]           = tempMinF;
    doc["tempMaxF"]           = tempMaxF;

    // Always include an interlock field so the UI can clear the red state
    if (sprayInterlockActive && lastInterlockReason.length() > 0) {
      doc["interlock"] = lastInterlockReason;
    } else {
      doc["interlock"] = nullptr;
    }

    size_t n = serializeJson(doc, statusJsonBuf, sizeof(statusJsonBuf));
    if (n > 0) {
      lastStatusJson = statusJsonBuf;
      webSocket.broadcastTXT(statusJsonBuf, n);
      HMISerial.write((const uint8_t*)statusJsonBuf, n);
      HMISerial.write('\n');
    }

    last = now;
  }


// --- Drive hose-tip LED according to hoseLedMode ---
  static bool hoseLedState = false;
  static unsigned long lastHoseLedToggle = 0;

  unsigned long interval = 0;

  switch (hoseLedMode) {
    case HOSE_LED_SOLID:
      // Solid ON whenever Iso HP is inside green band
      hoseLedWrite(HOSE_LED_MAX_DUTY);
      hoseLedState = true;
      break;

    case HOSE_LED_BLINK_SLOW:
      // Slow blink below green band
      interval = 700; // ms
      break;

    case HOSE_LED_BLINK_FAST:
      // Fast blink above band / setpoint
      interval = 220; // ms
      break;

    case HOSE_LED_PULSE: {
      // Rig standby: slow "breathing" pulse for controller-alive feedback
      static const uint32_t periodMs = 2600; // full fade in+out
      uint32_t t = now % periodMs;
      float phase = (float)t / (float)periodMs;                 // 0..1
      float level = 0.5f - 0.5f * cosf(6.2831853f * phase);     // 0..1
      uint32_t duty = (uint32_t)(level * (float)HOSE_LED_MAX_DUTY + 0.5f);
      hoseLedWrite(duty);
      hoseLedState = (duty > 0);
      break;
    }

    case HOSE_LED_OFF:
    default:
      hoseLedWrite(0);
      hoseLedState = false;
      break;
  }

  if (hoseLedMode == HOSE_LED_BLINK_SLOW || hoseLedMode == HOSE_LED_BLINK_FAST) {
    if (now - lastHoseLedToggle >= interval) {
      hoseLedState = !hoseLedState;
      hoseLedWrite(hoseLedState ? HOSE_LED_MAX_DUTY : 0);
      lastHoseLedToggle = now;
    }
  }
  // Temp read every ~1 s using assigned ROM addresses (no auto-pick)
  if (now - lastTempRead > 1000) {
    tempSensors.requestTemperatures();

    if (isoTempAssigned) {
      float tC = tempSensors.getTempC(isoTempAddr);
      if (tC > -100.0f) {
        isoTempF = tC * 9.0f / 5.0f + 32.0f;
      }
    }

    if (resinTempAssigned) {
      float tC = tempSensors.getTempC(resinTempAddr);
      if (tC > -100.0f) {
        resinTempF = tC * 9.0f / 5.0f + 32.0f;
      }
    }

    if (isoLowTempAssigned) {
      float tC = tempSensors.getTempC(isoLowTempAddr);
      if (tC > -100.0f) {
        isoLowTempF = tC * 9.0f / 5.0f + 32.0f;
      }
    }

    if (resinLowTempAssigned) {
      float tC = tempSensors.getTempC(resinLowTempAddr);
      if (tC > -100.0f) {
        resinLowTempF = tC * 9.0f / 5.0f + 32.0f;
      }
    }
    if (hose1TempAssigned) {
      float tC = tempSensors.getTempC(hose1TempAddr);
      if (tC > -100.0f) {
        hose1TempF = tC * 9.0f / 5.0f + 32.0f;
      }
    }

    if (hose2TempAssigned) {
      float tC = tempSensors.getTempC(hose2TempAddr);
      if (tC > -100.0f) {
        hose2TempF = tC * 9.0f / 5.0f + 32.0f;
      }
    }


    lastTempRead = now;
  }
}
