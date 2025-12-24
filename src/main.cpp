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

// Shared globals / constants used across modules
#include "rig_globals.h"

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

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
static inline void hoseLedInit() {
  // New API: ledcAttach(pin, freq, resolution_bits)
  ledcAttach(HOSE_LED_PIN, HOSE_LED_LEDC_FREQ, HOSE_LED_LEDC_BITS);
  ledcWrite(HOSE_LED_PIN, 0);
}
static inline void hoseLedWrite(uint32_t duty) {
  ledcWrite(HOSE_LED_PIN, duty);
}
#else
// Legacy API: ledcSetup(channel, freq, resolution_bits) + ledcAttachPin(pin, channel)
static inline void hoseLedWrite(uint32_t duty) {
  ledcWrite(HOSE_LED_LEDC_CH, duty);
}
static inline void hoseLedInit() {
  ledcSetup(HOSE_LED_LEDC_CH, HOSE_LED_LEDC_FREQ, HOSE_LED_LEDC_BITS);
  ledcAttachPin(HOSE_LED_PIN, HOSE_LED_LEDC_CH);
  hoseLedWrite(0);
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
// (enum NetworkModeType is declared in include/rig_globals.h)
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
String hmiRxBuffer;

// Last status JSON for HTTP fallback (/api/live)
String lastStatusJson;



// ---------- JSON helpers (ArduinoJson) ----------
// round1(...) lives in include/utils_math.h

// Capacity for the live status payload (pressures, temps, relay states, config, interlock).
// If you add lots of new fields later (e.g., hose heat zones), bump this.
// JSON_OBJECT_SIZE(32) is 704 bytes; we add headroom for strings and growth.
char statusJsonBuf[STATUS_JSON_OUT_MAX];



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

// Manual relay overrides (persisted). These are intended for troubleshooting
// and commissioning when spurious interlocks are suspected.
bool forceDrumAirOverride = false;
bool forceSprayOverride   = false;

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

String makeLowSupplyInterlockReason(float isoHP, float resinHP,
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

