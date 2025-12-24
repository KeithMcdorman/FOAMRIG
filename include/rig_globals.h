#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <HardwareSerial.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include "rig_pins.h"

// ---------- Core global objects ----------
extern WebServer        server;
extern WebSocketsServer webSocket;
extern Preferences      prefs;
extern HardwareSerial   HMISerial;

extern OneWire oneWire;
extern DallasTemperature tempSensors;

// UART RX buffer + last JSON snapshot (HTTP fallback)
extern String hmiRxBuffer;
extern String lastStatusJson;

// Status JSON buffer sizing (shared across modules)
// NOTE: keep these in one place so array sizing is visible in all translation units.
constexpr size_t STATUS_JSON_DOC_CAP = 2048;
constexpr size_t STATUS_JSON_OUT_MAX = 1024;

// Status JSON buffer (used by websocket + http)
extern char statusJsonBuf[STATUS_JSON_OUT_MAX];

// Network mode enum + defaults (defined in main.cpp)
enum NetworkModeType { NETMODE_AP = 0, NETMODE_STA = 1 };
extern const char* DEFAULT_AP_SSID;
extern const char* DEFAULT_AP_PASS;

// ---------- Network / fallback tracking ----------
extern int    networkMode;
extern String apSsid;
extern String apPass;
extern String staSsid;
extern String staPass;
extern unsigned long wifiDisconnSince;
extern bool          apFallbackActive;
extern const unsigned long WIFI_FALLBACK_MS;

// ---------- Settings (Preferences-backed) ----------
extern int  targetPressure;
extern int  marginPercent;
extern int  diffPressure;
extern int  airTarget;
extern int  gunTarget;
extern int  isoLowTarget;
extern int  resinLowTarget;
extern int  supplyLowPSI;

extern int  isoTempTargetF;
extern int  resinTempTargetF;
extern int  isoLowTempTargetF;
extern int  resinLowTempTargetF;
extern int  tempMinF;
extern int  tempMaxF;

// Calibration constants
extern float isoR0;
extern float resinR0;
extern float airR0;
extern float apAirR0;
extern float isoK;
extern float resinK;
extern float airK;
extern float apAirK;
extern float isoLowR0;
extern float isoLowK;
extern float resinLowR0;
extern float resinLowK;

// ---------- Relay state & pressure history ----------
extern bool drumAirEnabled;
extern bool sprayEnabled;

// Manual overrides (for troubleshooting / commissioning)
// When true, the relay output is forced ON regardless of alarms/interlocks.
extern bool forceDrumAirOverride;
extern bool forceSprayOverride;

extern float lastIsoLowPSI;
extern float lastResinLowPSI;
extern float lastIsoHPPSI;
extern float lastResinHPPSI;

// ---------- Hose heat control (2 zones) ----------
extern bool hose1Enabled;
extern bool hose2Enabled;
extern int hose1SetF;
extern int hose2SetF;
extern int hose1TolF;
extern int hose2TolF;
extern int hoseOvertempF;
extern bool hoseOvertempActive;
extern bool hose1Heating;
extern bool hose2Heating;
extern float hose1TempF;
extern float hose2TempF;

// DS18B20 assignments + readings
extern int   dsDeviceCount;
extern float isoTempF;
extern float resinTempF;
extern float isoLowTempF;
extern float resinLowTempF;

extern DeviceAddress isoTempAddr;
extern DeviceAddress resinTempAddr;
extern DeviceAddress isoLowTempAddr;
extern DeviceAddress resinLowTempAddr;
extern bool isoTempAssigned;
extern bool resinTempAssigned;
extern bool isoLowTempAssigned;
extern bool resinLowTempAssigned;

extern String isoTempAddrStr;
extern String resinTempAddrStr;
extern String isoLowTempAddrStr;
extern String resinLowTempAddrStr;

extern DeviceAddress hose1TempAddr;
extern DeviceAddress hose2TempAddr;
extern bool hose1TempAssigned;
extern bool hose2TempAssigned;
extern String hose1TempAddrStr;
extern String hose2TempAddrStr;

// ---------- Hose LED indicator ----------
enum HoseLedMode { HOSE_LED_OFF=0, HOSE_LED_SOLID=1, HOSE_LED_BLINK_SLOW=2, HOSE_LED_BLINK_FAST=3, HOSE_LED_PULSE=4 };
extern HoseLedMode hoseLedMode;

// ---------- Interlock state ----------
extern bool   sprayInterlockActive;
extern String lastInterlockReason;
