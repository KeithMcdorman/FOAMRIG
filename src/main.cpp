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

#include <OneWire.h>
#include <DallasTemperature.h>
#include <ctype.h>
#include <math.h>

// ---------- Firmware version ----------
const char* FW_VERSION = "V1.3";

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
// 25 = spray/park interlock, 26 = drum pump air supply
#define RELAY_SPRAY_PIN     25
#define RELAY_DRUM_AIR_PIN  26

// ---------- DS18B20 temperature support ----------
#define ONE_WIRE_BUS_PIN 27   // All four sensors on this pin

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

// ---------- Interlock state ----------
bool   sprayInterlockActive = false;
String lastInterlockReason;

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
  Serial.printf("Iso HP:      '%s'\r\n", isoTempAddrStr.c_str());
  Serial.printf("Resin HP:    '%s'\r\n", resinTempAddrStr.c_str());
  Serial.printf("Iso Low:     '%s'\r\n", isoLowTempAddrStr.c_str());
  Serial.printf("Resin Low:   '%s'\r\n", resinLowTempAddrStr.c_str());
  Serial.printf("Assigned flags: iso=%d resin=%d isoLow=%d resinLow=%d\r\n",
                isoTempAssigned, resinTempAssigned,
                isoLowTempAssigned, resinLowTempAssigned);

  dsDeviceCount = tempSensors.getDeviceCount();
  Serial.printf("Found %d DS18B20 device(s) on bus\r\n", dsDeviceCount);
  for (int i = 0; i < dsDeviceCount; i++) {
    DeviceAddress addr;
    if (tempSensors.getAddress(addr, i)) {
      String s = addressToString(addr);
      Serial.printf("  Index %d address: %s\r\n", i, s.c_str());
    } else {
      Serial.printf("  Index %d: <no address>\r\n", i);
    }
  }
  Serial.printf("Assigned Iso HP Temp:   %s\r\n", isoTempAddrStr.c_str());
  Serial.printf("Assigned Resin HP Temp: %s\r\n", resinTempAddrStr.c_str());
  Serial.printf("Assigned Iso Low Temp:  %s\r\n", isoLowTempAddrStr.c_str());
  Serial.printf("Assigned Resin Low Temp:%s\r\n", resinLowTempAddrStr.c_str());
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

    .top-row,
    .bottom-row {
      display: flex;
      align-items: stretch; /* stretch children to same height */
    }

    .top-row {
      justify-content: space-evenly;
    }

    .bottom-row {
      justify-content: space-between;
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
      width: 34vw;
      max-width: 380px;
      min-width: 260px;
    }

    .gauge-card.large.left,
    .gauge-card.large.right {
      margin-left: 0;
      margin-right: 0;
    }

    .gauge-card.large.left {
      transform: translateX(2.5vw);
    }

    .gauge-card.large.right {
      transform: translateX(-2.5vw);
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
      gap: 0.3rem;
      align-items: stretch;
      justify-content: stretch;
      width: 12vw;
      max-width: 180px;
      min-width: 110px;
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
        flex-direction: column;
        align-items: stretch;
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
        flex-direction: row;
        align-items: stretch;
      }
      .center-column > .mode-btn,
      .center-column > .ratio-card {
        flex: 1;
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
        <button id="sprayBtn" class="mode-btn">Spray</button>
        <div class="ratio-card">
          <div class="ratio-label-top">Ratio</div>
          <div id="ratioCircle" class="ratio-circle">
            <div class="ratio-inner">
              <div id="ratioText" class="ratio-text">RATIO</div>
            </div>
          </div>
        </div>
        <button id="drumAirBtn" class="mode-btn">Drum Air</button>
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
    <div class="footer-right">{{FW_VERSION}} • Temp bands + per-gauge targets + relay interlock + alert bar + OTA</div>
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

  var lastIsoLow   = 0;
  var lastResinLow = 0;

  var interlockActive = false;

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

    isoGauge.draw(isoGauge.value);
    resinGauge.draw(resinGauge.value);
    primaryAirGauge.draw(primaryAirGauge.value);
    gunAirGauge.draw(gunAirGauge.value);
    isoLowGauge.draw(isoLowGauge.value);
    resinLowGauge.draw(resinLowGauge.value);
  }).catch(function(e){ console.log('Settings load failed', e); });

  // Round gauge class with optional inner temp ring
  function RoundGauge(canvasId, maxVal) {
    this.canvas = document.getElementById(canvasId);
    this.ctx    = this.canvas.getContext('2d');
    this.max    = maxVal;
    this.value  = 0;
    this.size   = 0;
    this.setPoint = target;

    this.showTempRing = false;
    this.tempValue    = null; // °F
    this.tempMin      = 40;
    this.tempMax      = 180;
    this.tempSetPoint = null; // °F

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
      self.draw(self.value);
    };
    window.addEventListener('resize', this.resize);
    this.resize();
  }

  RoundGauge.prototype.setValue = function(v) {
    if (v < 0) v = 0;
    if (v > this.max) v = this.max;
    this.value = v;
    this.draw(v);
  };

  RoundGauge.prototype.setTemp = function(tF) {
    this.tempValue = tF;
    if (this.showTempRing) {
      this.draw(this.value);
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

    // Inner temperature ring, mapped to same arc, with ticks & band
    if (this.showTempRing) {
      var tMin   = this.tempMin;
      var tMax   = this.tempMax;
      if (tMax <= tMin) {
        tMin = 40; tMax = 180;
      }
      var innerR = r - 10;

      // Draw full temperature range background
      ctx.beginPath();
      ctx.arc(cx, cy, innerR, start, start + span);
      ctx.lineWidth = 3;
      ctx.strokeStyle = '#1e293b';
      ctx.stroke();

      // Temp green band around tempSetPoint using same % margin
      if (this.tempSetPoint !== null && typeof this.tempSetPoint === 'number') {
        var spT = this.tempSetPoint;
        var mT  = mPct;
        var lowT  = spT * (1 - mT);
        var highT = spT * (1 + mT);
        if (lowT < tMin)  lowT  = tMin;
        if (highT > tMax) highT = tMax;

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

      // Actual temperature segment on top of the band
      if (this.tempValue !== null) {
        var t = this.tempValue;
        if (t < tMin) t = tMin;
        if (t > tMax) t = tMax;
        var tFrac  = (t - tMin) / (tMax - tMin);
        var tAngle = start + tFrac * span;
        ctx.beginPath();
        ctx.arc(cx, cy, innerR, start, tAngle);
        ctx.lineWidth = 4;
        ctx.strokeStyle = '#38bdf8';
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
    var drumBtn = document.getElementById('drumAirBtn');
    var sprayBtn = document.getElementById('sprayBtn');

    if (drumAir) {
      drumBtn.classList.add('on');
    } else {
      drumBtn.classList.remove('on');
    }

    if (spray) {
      sprayBtn.classList.add('on');
    } else {
      sprayBtn.classList.remove('on');
    }

    if (interlockActive) {
      sprayBtn.classList.add('interlock');
    } else {
      sprayBtn.classList.remove('interlock');
    }
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

      if (typeof body.interlock !== 'undefined' && body.interlock) {
        // Interlock still active
        interlockActive = true;
        setStatus(body.interlock, true);
      } else {
        // Successful command with no interlock -> clear the red state
        interlockActive = false;
      }

      updateModeButtons();
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

  function connectWS() {
    var ws = new WebSocket('ws://' + location.hostname + ':81/');
    ws.onopen = function() {
      setLiveStatus(true);
    };
    ws.onmessage = function(ev) {
      try {
        var d = JSON.parse(ev.data);
        updateGauges(d);
      } catch (e) {
        console.log('Bad WS payload', e);
      }
    };
    ws.onclose = function() {
      setLiveStatus(false);
      setTimeout(connectWS, 1500);
    };
    ws.onerror = function() {
      setLiveStatus(false);
    };
  }

  // Initial fetch of relay state
  fetch('/api/control').then(function(r){ return r.json(); }).then(function(data){
    if (typeof data.drumAir !== 'undefined') drumAir = !!data.drumAir;
    if (typeof data.spray   !== 'undefined') spray   = !!data.spray;
    updateModeButtons();
  }).catch(function(e){
    console.log('Failed to load relay state:', e);
  });

  connectWS();
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

    var isoSpan      = document.getElementById('isoTempReading');
    var resinSpan    = document.getElementById('resinTempReading');
    var isoLowSpan   = document.getElementById('isoLowTempReading');
    var resinLowSpan = document.getElementById('resinLowTempReading');

    var isoId       = isoSel.value;
    var resinId     = resinSel.value;
    var isoLowId    = isoLowSel.value;
    var resinLowId  = resinLowSel.value;

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

          sensors.forEach(function(s, idx){
            var label = 'Sensor ' + idx + ' (' + s.id + ')';
            if (typeof s.tempF === 'number') {
              label += ' – ' + s.tempF.toFixed(1) + ' °F';
            } else {
              label += ' – n/a';
            }

            [isoSel, resinSel, isoLowSel, resinLowSel].forEach(function(sel){
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

    var wifiModeVal = parseInt(document.getElementById('wifiModeSelect').value || '0', 10);
    var apSsidVal   = document.getElementById('apSsidInput').value || '';
    var apPassVal   = document.getElementById('apPassInput').value || '';
    var staSsidVal  = document.getElementById('staSsidInput').value || '';
    var staPassVal  = document.getElementById('staPassInput').value || '';

    var isoTempId       = document.getElementById('isoTempSensorSelect').value;
    var resinTempId     = document.getElementById('resinTempSensorSelect').value;
    var isoLowTempId    = document.getElementById('isoLowTempSensorSelect').value;
    var resinLowTempId  = document.getElementById('resinLowTempSensorSelect').value;

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
      resinLow: resinLowTempId
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

// /api/settings
void handleSettings() {
  if (server.method() == HTTP_GET) {
    DynamicJsonDocument doc(896);
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

    doc["wifiMode"] = networkMode;
    doc["apSsid"]   = apSsid;
    doc["apPass"]   = apPass;
    doc["staSsid"]  = staSsid;
    doc["staPass"]  = staPass;

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  } else if (server.method() == HTTP_POST) {
    DynamicJsonDocument doc(512);
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

    if (doc.containsKey("wifiMode")) {
      networkMode = (int)doc["wifiMode"];
      if (networkMode != NETMODE_AP && networkMode != NETMODE_STA) {
        networkMode = NETMODE_AP;
      }
    }
    if (doc.containsKey("apSsid")) {
      const char* v = doc["apSsid"];
      if (v) apSsid = String(v);
    }
    if (doc.containsKey("apPass")) {
      const char* v = doc["apPass"];
      if (v) apPass = String(v);
    }
    if (doc.containsKey("staSsid")) {
      const char* v = doc["staSsid"];
      if (v) staSsid = String(v);
    }
    if (doc.containsKey("staPass")) {
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
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  DynamicJsonDocument out(256);
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
    out["ok"] = true;
    out["raw0"] = raw0;
  } else if (!strcmp(action, "span")) {
    if (!doc.containsKey("pressure")) {
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
  DynamicJsonDocument doc(512);
  JsonObject iso      = doc.createNestedObject("iso");      iso["R0"] = isoR0;       iso["K"] = isoK;
  JsonObject resin    = doc.createNestedObject("resin");    resin["R0"] = resinR0;   resin["K"] = resinK;
  JsonObject isoLow   = doc.createNestedObject("isoLow");   isoLow["R0"] = isoLowR0; isoLow["K"] = isoLowK;
  JsonObject resinLow = doc.createNestedObject("resinLow"); resinLow["R0"] = resinLowR0; resinLow["K"] = resinLowK;
  JsonObject air      = doc.createNestedObject("air");      air["R0"] = airR0;       air["K"] = airK;
  JsonObject apAir    = doc.createNestedObject("apAir");    apAir["R0"] = apAirR0;   apAir["K"] = apAirK;
  String s;
  serializeJson(doc, s);
  server.send(200, "application/json", s);
}

// /api/temp-sensors
void handleTempSensors() {
  if (server.method() == HTTP_GET) {
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("sensors");

    dsDeviceCount = tempSensors.getDeviceCount();
    DeviceAddress addr;

    // IMPORTANT: we do NOT call requestTemperatures() here.
    // We just read last-converted values to avoid blocking and lag.

    for (int i = 0; i < dsDeviceCount; i++) {
      if (tempSensors.getAddress(addr, i)) {
        String id = addressToString(addr);
        JsonObject sObj = arr.createNestedObject();
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

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  }
  else if (server.method() == HTTP_POST) {
    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
      server.send(400, "text/plain", "Invalid JSON");
      return;
    }

    const char* isoStr      = doc["iso"]      | "";
    const char* resinStr    = doc["resin"]    | "";
    const char* isoLowStr   = doc["isoLow"]   | "";
    const char* resinLowStr = doc["resinLow"] | "";

    isoTempAddrStr      = String(isoStr);
    resinTempAddrStr    = String(resinStr);
    isoLowTempAddrStr   = String(isoLowStr);
    resinLowTempAddrStr = String(resinLowStr);

    isoTempAssigned      = parseAddressString(isoTempAddrStr,      isoTempAddr);
    resinTempAssigned    = parseAddressString(resinTempAddrStr,    resinTempAddr);
    isoLowTempAssigned   = parseAddressString(isoLowTempAddrStr,   isoLowTempAddr);
    resinLowTempAssigned = parseAddressString(resinLowTempAddrStr, resinLowTempAddr);

    prefs.putString("isoTempAddr",    isoTempAddrStr);
    prefs.putString("resTempAddr",    resinTempAddrStr);
    prefs.putString("isoLowTempAddr", isoLowTempAddrStr);
    prefs.putString("resLowTempAddr", resinLowTempAddrStr);

    Serial.println("=== Temp assignments UPDATED via /api/temp-sensors ===");
    printDS18B20Addresses();

    DynamicJsonDocument outDoc(128);
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
    DynamicJsonDocument doc(128);
    doc["drumAir"] = drumAirEnabled;
    doc["spray"]   = sprayEnabled;
    String s;
    serializeJson(doc, s);
    server.send(200, "application/json", s);
    return;
  }

  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  DynamicJsonDocument out(256);
  if (err) {
    out["ok"] = false;
    out["error"] = "Invalid JSON";
    String s;
    serializeJson(out, s);
    server.send(400, "application/json", s);
    return;
  }

  if (doc.containsKey("drumAir")) {
    bool desired = doc["drumAir"];
    drumAirEnabled = desired;
    digitalWrite(RELAY_DRUM_AIR_PIN, drumAirEnabled ? HIGH : LOW);
    if (!drumAirEnabled) {
      // if you kill drum air, also drop spray as a safety
      sprayEnabled = false;
      digitalWrite(RELAY_SPRAY_PIN, LOW);
    }
  }

  if (doc.containsKey("spray")) {
    bool desired = doc["spray"];

    if (desired) {
      // Guard: only allow spray if both low sides above threshold and drum air is on
      if (!(lastIsoLowPSI >= supplyLowPSI && lastResinLowPSI >= supplyLowPSI)) {
        // Latch an interlock for low supply
        sprayInterlockActive = true;
        lastInterlockReason  = "Interlock: low supply pressure on feed side.";

        out["ok"]        = false;
        out["error"]     = "Low supply pressure";
        out["drumAir"]   = drumAirEnabled;
        out["spray"]     = sprayEnabled;
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

  out["ok"]      = true;
  out["drumAir"] = drumAirEnabled;
  out["spray"]   = sprayEnabled;
  String s;
  serializeJson(out, s);
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

  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_A_PIN,         ADC_11db);
  analogSetPinAttenuation(SENSOR_B_PIN,         ADC_11db);
  analogSetPinAttenuation(SENSOR_AIRPISTON_PIN, ADC_11db);
  analogSetPinAttenuation(SENSOR_APAIR_PIN,     ADC_11db);
  analogSetPinAttenuation(SENSOR_ISO_LOW_PIN,    ADC_11db);
  analogSetPinAttenuation(SENSOR_RESIN_LOW_PIN,  ADC_11db);

  // Relay outputs
  pinMode(RELAY_SPRAY_PIN, OUTPUT);
  pinMode(RELAY_DRUM_AIR_PIN, OUTPUT);
  digitalWrite(RELAY_SPRAY_PIN, LOW);
  digitalWrite(RELAY_DRUM_AIR_PIN, LOW);

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

  isoTempAssigned      = parseAddressString(isoTempAddrStr,      isoTempAddr);
  resinTempAssigned    = parseAddressString(resinTempAddrStr,    resinTempAddr);
  isoLowTempAssigned   = parseAddressString(isoLowTempAddrStr,   isoLowTempAddr);
  resinLowTempAssigned = parseAddressString(resinLowTempAddrStr, resinLowTempAddr);

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

void loop() {
  server.handleClient();
  webSocket.loop();
  handleWifiFallback();

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
      lastInterlockReason  = "Interlock: low supply pressure on feed side.";
    }

    String json = "{\"iso\":"      + String(isoPSI,1) +
                  ",\"resin\":"    + String(resinPSI,1) +
                  ",\"isoLow\":"   + String(isoLowPSI,1) +
                  ",\"resinLow\":" + String(resinLowPSI,1) +
                  ",\"airPiston\":"+ String(airPistonPSI,1) +
                  ",\"apAir\":"    + String(apAirPSI,1);

    if (!isnan(isoTempF)) {
      json += ",\"isoTemp\":" + String(isoTempF,1);
    }
    if (!isnan(resinTempF)) {
      json += ",\"resinTemp\":" + String(resinTempF,1);
    }
    if (!isnan(isoLowTempF)) {
      json += ",\"isoLowTemp\":" + String(isoLowTempF,1);
    }
    if (!isnan(resinLowTempF)) {
      json += ",\"resinLowTemp\":" + String(resinLowTempF,1);
    }

    json += ",\"drumAir\":"; json += drumAirEnabled ? "1" : "0";
    json += ",\"spray\":";   json += sprayEnabled   ? "1" : "0";

    // Always include an interlock field so the UI can clear the red state
    json += ",\"interlock\":";
    if (sprayInterlockActive && lastInterlockReason.length() > 0) {
      String safeReason = lastInterlockReason;
      safeReason.replace("\"", "'");
      json += "\"";
      json += safeReason;
      json += "\"";
    } else {
      json += "null";
    }

    json += "}";
    webSocket.broadcastTXT(json);
    last = now;
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

    lastTempRead = now;
  }
}
