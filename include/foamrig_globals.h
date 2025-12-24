#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <HardwareSerial.h>
#include <OneWire.h>
#include <DallasTemperature.h>

extern const char* FW_VERSION;

extern WebServer        server;
extern WebSocketsServer webSocket;
extern Preferences      prefs;

extern HardwareSerial   HMISerial;

extern OneWire oneWire;
extern DallasTemperature tempSensors;

extern int dsDeviceCount;
extern float isoTempF;
extern float resinTempF;
extern float isoLowTempF;
extern float resinLowTempF;

extern uint8_t isoTempAddr[8];
extern uint8_t resinTempAddr[8];
extern uint8_t isoLowTempAddr[8];
extern uint8_t resinLowTempAddr[8];

extern bool isoTempAssigned;
extern bool resinTempAssigned;
extern bool isoLowTempAssigned;
extern bool resinLowTempAssigned;

extern String isoTempAddrStr;
extern String resinTempAddrStr;
extern String isoLowTempAddrStr;
extern String resinLowTempAddrStr;

extern int networkMode;
extern String apSsid;
extern String apPass;
extern String staSsid;
extern String staPass;

extern unsigned long wifiDisconnSince;
extern bool apFallbackActive;
extern const unsigned long WIFI_FALLBACK_MS;

extern String lastStatusJson;

extern int targetPressure;
extern int marginPercent;
extern int diffPressure;
extern int airTarget;
extern int gunTarget;
extern int isoLowTarget;
extern int resinLowTarget;
extern int supplyLowPSI;

extern int isoTempTargetF;
extern int resinTempTargetF;
extern int isoLowTempTargetF;
extern int resinLowTempTargetF;
extern int tempMinF;
extern int tempMaxF;

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

extern bool drumAirEnabled;
extern bool sprayEnabled;

// Manual relay overrides (Settings-driven)
extern bool forceDrumAirOverride;
extern bool forceSprayOverride;

extern float lastIsoLowPSI;
extern float lastResinLowPSI;
extern float lastIsoHPPSI;
extern float lastResinHPPSI;

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
extern uint8_t hose1TempAddr[8];
extern uint8_t hose2TempAddr[8];
extern bool hose1TempAssigned;
extern bool hose2TempAssigned;
extern String hose1TempAddrStr;
extern String hose2TempAddrStr;

enum HoseLedMode { HOSE_LED_OFF=0, HOSE_LED_SOLID=1, HOSE_LED_BLINK_SLOW=2, HOSE_LED_BLINK_FAST=3, HOSE_LED_PULSE=4 };
extern HoseLedMode hoseLedMode;

extern bool   sprayInterlockActive;
extern String lastInterlockReason;
