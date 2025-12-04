/*
  CNMC-E3 Spray Foam Proportioner – Headless Rig Controller
  Version: Rig V1.5-SYNC (FULLY WORKING WITH CYD HMI)
  All settings, calibration, and DS18B20 assignments are now 100% synchronized over UART
*/

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// UART to CYD HMI
HardwareSerial RigSerial(2);
#define RIG_UART_RX  16   // Connects to CYD TX (GPIO1)
#define RIG_UART_TX  17   // Connects to CYD RX (GPIO3)
#define RIG_UART_BAUD 115200

// OTA state
bool    rigOtaActive = false;
size_t  rigOtaBytesRemaining = 0;

const char* FW_VERSION = "Rig V1.5-SYNC";

// ---------- Pins ----------
#define SENSOR_A_PIN         35   // Iso High Pressure
#define SENSOR_B_PIN         34   // Resin High Pressure
#define SENSOR_AIRPISTON_PIN 32   // Primary air piston
#define SENSOR_APAIR_PIN     33   // Gun AP air
#define SENSOR_ISO_LOW_PIN   36   // Iso low-side feed
#define SENSOR_RESIN_LOW_PIN 39   // Resin low-side feed

#define RELAY_SPRAY_PIN      25
#define RELAY_DRUM_AIR_PIN   26

#define ONE_WIRE_BUS_PIN     27
OneWire oneWire(ONE_WIRE_BUS_PIN);
DallasTemperature tempSensors(&oneWire);

// ---------- Temperature sensors ----------
DeviceAddress isoTempAddr, resinTempAddr, isoLowTempAddr, resinLowTempAddr;
bool isoTempAssigned = false, resinTempAssigned = false;
bool isoLowTempAssigned = false, resinLowTempAssigned = false;
String isoTempAddrStr, resinTempAddrStr, isoLowTempAddrStr, resinLowTempAddrStr;

float isoTempF = NAN, resinTempF = NAN, isoLowTempF = NAN, resinLowTempF = NAN;

// ---------- Settings (all saved in NVS) ----------
int targetPressure = 1000, marginPercent = 10, diffPressure = 50;
int airTarget = 100, gunTarget = 100;
int isoLowTarget = 200, resinLowTarget = 200, supplyLowPSI = 150;
int isoTempTargetF = 120, resinTempTargetF = 120;
int isoLowTempTargetF = 120, resinLowTempTargetF = 120;
int tempMinF = 40, tempMaxF = 180;

// Calibration
float isoR0 = 0, isoK = 1, resinR0 = 0, resinK = 1;
float isoLowR0 = 0, isoLowK = 1, resinLowR0 = 0, resinLowK = 1;
float airR0 = 0, airK = 1, apAirR0 = 0, apAirK = 1;

// Runtime state
bool drumAirEnabled = false, sprayEnabled = false;
float lastIsoLowPSI = 0, lastResinLowPSI = 0;
bool sprayInterlockActive = false;
String lastInterlockReason = "";

Preferences prefs;

// ---------- Helper functions ----------
String addrToStr(const uint8_t addr[8]) {
  char buf[17];
  for (int i = 0; i < 8; i++) sprintf(&buf[i*2], "%02X", addr[i]);
  buf[16] = '\0';
  return String(buf);
}

bool strToAddr(const String& s, uint8_t addr[8]) {
  if (s.length() != 16) return false;
  for (int i = 0; i < 8; i++) {
    char h = s[2*i], l = s[2*i+1];
    int hi = (h>='0'&&h<='9') ? h-'0' : (toupper(h)-'A'+10);
    int lo = (l>='0'&&l<='9') ? l-'0' : (toupper(l)-'A'+10);
    if (hi < 0 || lo < 0) return false;
    addr[i] = (hi << 4) | lo;
  }
  return true;
}

float readRawPSI(int pin, float fullScale) {
  const int samples = 8;
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  float adc = sum / samples;
  float vEsp = (adc / 4095.0f) * 3.3f;
  const float Rtop = 10000.0f, Rbot = 22000.0f;
  float vSensor = vEsp / (Rbot / (Rtop + Rbot));
  float psi = (vSensor - 0.5f) * (fullScale / 4.0f);
  return psi;
}

float applyCal(float raw, float R0, float K, float fullScale) {
  float p = (raw - R0) * K;
  if (p < 0) p = 0;
  if (p > fullScale) p = fullScale;
  return p;
}

// ---------- UART command handlers ----------
void handleControl(const JsonDocument& doc) {
  DynamicJsonDocument out(256);
  if (doc.containsKey("drumAir")) {
    drumAirEnabled = doc["drumAir"];
    digitalWrite(RELAY_DRUM_AIR_PIN, drumAirEnabled ? HIGH : LOW);
    if (!drumAirEnabled) {
      sprayEnabled = false;
      digitalWrite(RELAY_SPRAY_PIN, LOW);
    }
  }
  if (doc.containsKey("spray")) {
    bool want = doc["spray"];
    if (want) {
      if (lastIsoLowPSI < supplyLowPSI || lastResinLowPSI < supplyLowPSI) {
        sprayInterlockActive = true; lastInterlockReason = "Low supply pressure";
        out["ok"] = false; out["error"] = "Low supply";
      } else if (!drumAirEnabled) {
        sprayInterlockActive = true; lastInterlockReason = "Drum air off";
        out["ok"] = false; out["error"] = "Drum air required";
      } else {
        sprayEnabled = true;
        digitalWrite(RELAY_SPRAY_PIN, HIGH);
        sprayInterlockActive = false;
        lastInterlockReason = "";
      }
    } else {
      sprayEnabled = false;
      digitalWrite(RELAY_SPRAY_PIN, LOW);
    }
  }
  out["ok"] = true;
  out["drumAir"] = drumAirEnabled;
  out["spray"] = sprayEnabled;
  if (sprayInterlockActive) out["interlock"] = lastInterlockReason;
  out["resp"] = "control";
  String s; serializeJson(out, s); RigSerial.println(s);
}

void handleGetSettings() {
  DynamicJsonDocument doc(2048);
  doc["targetPressure"] = targetPressure; doc["marginPercent"] = marginPercent; doc["diffPressure"] = diffPressure;
  doc["airTarget"] = airTarget; doc["gunTarget"] = gunTarget;
  doc["isoLowTarget"] = isoLowTarget; doc["resinLowTarget"] = resinLowTarget; doc["supplyLowPSI"] = supplyLowPSI;
  doc["isoTempTargetF"] = isoTempTargetF; doc["resinTempTargetF"] = resinTempTargetF;
  doc["isoLowTempTargetF"] = isoLowTempTargetF; doc["resinLowTempTargetF"] = resinLowTempTargetF;
  doc["tempMinF"] = tempMinF; doc["tempMaxF"] = tempMaxF;

  JsonObject c = doc.createNestedObject("cal");
  c["isoR0"] = isoR0; c["isoK"] = isoK;
  c["resinR0"] = resinR0; c["resinK"] = resinK;
  c["isoLowR0"] = isoLowR0; c["isoLowK"] = isoLowK;
  c["resinLowR0"] = resinLowR0; c["resinLowK"] = resinLowK;
  c["airR0"] = airR0; c["airK"] = airK;
  c["apAirR0"] = apAirR0; c["apAirK"] = apAirK;

  doc["isoTempAddr"] = isoTempAddrStr;
  doc["resinTempAddr"] = resinTempAddrStr;
  doc["isoLowTempAddr"] = isoLowTempAddrStr;
  doc["resinLowTempAddr"] = resinLowTempAddrStr;

  doc["resp"] = "settings";
  String out; serializeJson(doc, out);
  RigSerial.println(out);
}

void handleSetSettings(const JsonDocument& doc) {
  if (doc.containsKey("targetPressure")) targetPressure = doc["targetPressure"];
  if (doc.containsKey("marginPercent")) marginPercent = doc["marginPercent"];
  if (doc.containsKey("diffPressure")) diffPressure = doc["diffPressure"];
  if (doc.containsKey("airTarget")) airTarget = doc["airTarget"];
  if (doc.containsKey("gunTarget")) gunTarget = doc["gunTarget"];
  if (doc.containsKey("isoLowTarget")) isoLowTarget = doc["isoLowTarget"];
  if (doc.containsKey("resinLowTarget")) resinLowTarget = doc["resinLowTarget"];
  if (doc.containsKey("supplyLowPSI")) supplyLowPSI = doc["supplyLowPSI"];
  if (doc.containsKey("isoTempTargetF")) isoTempTargetF = doc["isoTempTargetF"];
  if (doc.containsKey("resinTempTargetF")) resinTempTargetF = doc["resinTempTargetF"];
  if (doc.containsKey("isoLowTempTargetF")) isoLowTempTargetF = doc["isoLowTempTargetF"];
  if (doc.containsKey("resinLowTempTargetF")) resinLowTempTargetF = doc["resinLowTempTargetF"];
  if (doc.containsKey("tempMinF")) tempMinF = doc["tempMinF"];
  if (doc.containsKey("tempMaxF")) tempMaxF = doc["tempMaxF"];

  auto cal = doc["cal"];
  if (cal.containsKey("isoR0"))    { isoR0 = cal["isoR0"]; isoK = cal["isoK"]; }
  if (cal.containsKey("resinR0"))  { resinR0 = cal["resinR0"]; resinK = cal["resinK"]; }
  if (cal.containsKey("isoLowR0")) { isoLowR0 = cal["isoLowR0"]; isoLowK = cal["isoLowK"]; }
  if (cal.containsKey("resinLowR0")){ resinLowR0 = cal["resinLowR0"]; resinLowK = cal["resinLowK"]; }
  if (cal.containsKey("airR0"))    { airR0 = cal["airR0"]; airK = cal["airK"]; }
  if (cal.containsKey("apAirR0"))  { apAirR0 = cal["apAirR0"]; apAirK = cal["apAirK"]; }

  if (doc.containsKey("isoTempAddr"))    isoTempAddrStr = doc["isoTempAddr"].as<String>();
  if (doc.containsKey("resinTempAddr"))  resinTempAddrStr = doc["resinTempAddr"].as<String>();
  if (doc.containsKey("isoLowTempAddr")) isoLowTempAddrStr = doc["isoLowTempAddr"].as<String>();
  if (doc.containsKey("resinLowTempAddr")) resinLowTempAddrStr = doc["resinLowTempAddr"].as<String>();

  isoTempAssigned      = strToAddr(isoTempAddrStr, isoTempAddr);
  resinTempAssigned    = strToAddr(resinTempAddrStr, resinTempAddr);
  isoLowTempAssigned   = strToAddr(isoLowTempAddrStr, isoLowTempAddr);
  resinLowTempAssigned = strToAddr(resinLowTempAddrStr, resinLowTempAddr);

  // Save everything to NVS
  prefs.putInt("target", targetPressure);
  prefs.putInt("margin", marginPercent);
  prefs.putInt("diff", diffPressure);
  prefs.putInt("airTarget", airTarget);
  prefs.putInt("gunTarget", gunTarget);
  prefs.putInt("isoLowTarget", isoLowTarget);
  prefs.putInt("resinLowTarget", resinLowTarget);
  prefs.putInt("supplyLow", supplyLowPSI);
  prefs.putInt("isoTempTarget", isoTempTargetF);
  prefs.putInt("resTempTarget", resinTempTargetF);
  prefs.putInt("isoLowTempTgt", isoLowTempTargetF);
  prefs.putInt("resLowTempTgt", resinLowTempTargetF);
  prefs.putInt("tempMinF", tempMinF);
  prefs.putInt("tempMaxF", tempMaxF);

  prefs.putFloat("iso_R0", isoR0); prefs.putFloat("iso_K", isoK);
  prefs.putFloat("resin_R0", resinR0); prefs.putFloat("resin_K", resinK);
  prefs.putFloat("isoLow_R0", isoLowR0); prefs.putFloat("isoLow_K", isoLowK);
  prefs.putFloat("resinLow_R0", resinLowR0); prefs.putFloat("resinLow_K", resinLowK);
  prefs.putFloat("air_R0", airR0); prefs.putFloat("air_K", airK);
  prefs.putFloat("apAir_R0", apAirR0); prefs.putFloat("apAir_K", apAirK);

  prefs.putString("isoTempAddr", isoTempAddrStr);
  prefs.putString("resTempAddr", resinTempAddrStr);
  prefs.putString("isoLowTempAddr", isoLowTempAddrStr);
  prefs.putString("resLowTempAddr", resinLowTempAddrStr);

  DynamicJsonDocument out(128);
  out["resp"] = "settings_saved";
  out["ok"] = true;
  String s; serializeJson(out, s);
  RigSerial.println(s);
}

// ---------- UART handler ----------
void handleRigSerial() {
  static String rxLine = "";
  while (RigSerial.available()) {
    char c = RigSerial.read();
    if (c == '\n') {
      rxLine.trim();
      if (rxLine.length() == 0) { rxLine = ""; continue; }

      // OTA handling (unchanged from your original code – keep it exactly as you had)
      if (rxLine.startsWith("RIG_OTA_BEGIN ")) {
        rigOtaBytesRemaining = strtoul(rxLine.c_str() + 14, nullptr, 10);
        if (rigOtaBytesRemaining && Update.begin(rigOtaBytesRemaining)) {
          rigOtaActive = true;
          RigSerial.println("RIG_OTA_READY");
        } else {
          RigSerial.println("RIG_OTA_ERROR BEGIN");
        }
        rxLine = "";
        continue;
      }

      if (rigOtaActive) {
        if (Update.write((uint8_t*)rxLine.c_str(), rxLine.length()) != rxLine.length()) {
          Update.printError(Serial);
          RigSerial.println("RIG_OTA_ERROR WRITE");
          rigOtaActive = false;
        }
        rigOtaBytesRemaining -= rxLine.length();
        if (rigOtaBytesRemaining == 0) {
          if (Update.end(true)) {
            RigSerial.println("RIG_OTA_DONE");
            delay(200);
            ESP.restart();
          } else {
            RigSerial.println("RIG_OTA_ERROR END");
            rigOtaActive = false;
          }
        }
        rxLine = "";
        continue;
      }

      // Normal JSON commands
      if (rxLine[0] == '{') {
        StaticJsonDocument<2048> doc;
        DeserializationError err = deserializeJson(doc, rxLine);
        if (!err) {
          const char* cmd = doc["cmd"] | "";
          if (strcmp(cmd, "control") == 0)       handleControl(doc);
          else if (strcmp(cmd, "get_settings") == 0) handleGetSettings();
          else if (strcmp(cmd, "set_settings") == 0) handleSetSettings(doc);
          else if (strcmp(cmd, "ping") == 0)     RigSerial.println("{\"resp\":\"pong\"}");
        }
      }
      rxLine = "";
    } else if (c != '\r') {
      rxLine += c;
    }
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Foam Rig V1.5-SYNC ===");

  RigSerial.begin(RIG_UART_BAUD, SERIAL_8N1, RIG_UART_RX, RIG_UART_TX);

  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_A_PIN, ADC_11db);
  analogSetPinAttenuation(SENSOR_B_PIN, ADC_11db);
  analogSetPinAttenuation(SENSOR_AIRPISTON_PIN, ADC_11db);
  analogSetPinAttenuation(SENSOR_APAIR_PIN, ADC_11db);
  analogSetPinAttenuation(SENSOR_ISO_LOW_PIN, ADC_11db);
  analogSetPinAttenuation(SENSOR_RESIN_LOW_PIN, ADC_11db);

  pinMode(RELAY_SPRAY_PIN, OUTPUT);
  pinMode(RELAY_DRUM_AIR_PIN, OUTPUT);
  digitalWrite(RELAY_SPRAY_PIN, LOW);
  digitalWrite(RELAY_DRUM_AIR_PIN, LOW);

  tempSensors.begin();
  prefs.begin("foam", false);

  // Load all saved settings
  targetPressure   = prefs.getInt("target", 1000);
  marginPercent    = prefs.getInt("margin", 10);
  diffPressure     = prefs.getInt("diff", 50);
  airTarget        = prefs.getInt("airTarget", 100);
  gunTarget        = prefs.getInt("gunTarget", 100);
  isoLowTarget     = prefs.getInt("isoLowTarget", 200);
  resinLowTarget   = prefs.getInt("resinLowTarget", 200);
  supplyLowPSI     = prefs.getInt("supplyLow", 150);
  isoTempTargetF   = prefs.getInt("isoTempTarget", 120);
  resinTempTargetF = prefs.getInt("resTempTarget", 120);
  isoLowTempTargetF   = prefs.getInt("isoLowTempTgt", 120);
  resinLowTempTargetF = prefs.getInt("resLowTempTgt", 120);
  tempMinF = prefs.getInt("tempMinF", 40);
  tempMaxF = prefs.getInt("tempMaxF", 180);

  isoR0 = prefs.getFloat("iso_R0", 0); isoK = prefs.getFloat("iso_K", 1);
  resinR0 = prefs.getFloat("resin_R0", 0); resinK = prefs.getFloat("resin_K", 1);
  isoLowR0 = prefs.getFloat("isoLow_R0", 0); isoLowK = prefs.getFloat("isoLow_K", 1);
  resinLowR0 = prefs.getFloat("resinLow_R0", 0); resinLowK = prefs.getFloat("resinLow_K", 1);
  airR0 = prefs.getFloat("air_R0", 0); airK = prefs.getFloat("air_K", 1);
  apAirR0 = prefs.getFloat("apAir_R0", 0); apAirK = prefs.getFloat("apAir_K", 1);

  isoTempAddrStr    = prefs.getString("isoTempAddr", "");
  resinTempAddrStr  = prefs.getString("resTempAddr", "");
  isoLowTempAddrStr = prefs.getString("isoLowTempAddr", "");
  resinLowTempAddrStr = prefs.getString("resLowTempAddr", "");

  isoTempAssigned      = strToAddr(isoTempAddrStr, isoTempAddr);
  resinTempAssigned    = strToAddr(resinTempAddrStr, resinTempAddr);
  isoLowTempAssigned   = strToAddr(isoLowTempAddrStr, isoLowTempAddr);
  resinLowTempAssigned = strToAddr(resinLowTempAddrStr, resinLowTempAddr);
}

// ---------- Main loop ----------
void loop() {
  handleRigSerial();
  if (rigOtaActive) return;

  static unsigned long last = 0, lastTemp = 0;
  unsigned long now = millis();

  // ~200ms telemetry
  if (now - last >= 200) {
    float rawIso    = readRawPSI(SENSOR_A_PIN, 1600);
    float rawResin  = readRawPSI(SENSOR_B_PIN, 1600);
    float rawIsoLow = readRawPSI(SENSOR_ISO_LOW_PIN, 500);
    float rawResinLow = readRawPSI(SENSOR_RESIN_LOW_PIN, 500);
    float rawAir    = readRawPSI(SENSOR_AIRPISTON_PIN, 300);
    float rawApAir  = readRawPSI(SENSOR_APAIR_PIN, 300);

    float isoPSI    = applyCal(rawIso, isoR0, isoK, 1600);
    float resinPSI  = applyCal(rawResin, resinR0, resinK, 1600);
    float isoLowPSI = applyCal(rawIsoLow, isoLowR0, isoLowK, 500);
    float resinLowPSI = applyCal(rawResinLow, resinLowR0, resinLowK, 500);
    float airPSI    = applyCal(rawAir, airR0, airK, 300);
    float apAirPSI  = applyCal(rawApAir, apAirR0, apAirK, 300);

    lastIsoLowPSI = isoLowPSI;
    lastResinLowPSI = resinLowPSI;

    // Auto-interlock on low supply
    if (sprayEnabled && (isoLowPSI < supplyLowPSI || resinLowPSI < supplyLowPSI)) {
      sprayEnabled = drumAirEnabled = false;
      digitalWrite(RELAY_SPRAY_PIN, LOW);
      digitalWrite(RELAY_DRUM_AIR_PIN, LOW);
      sprayInterlockActive = true;
      lastInterlockReason = "Low supply pressure";
    }

    String json = "{\"iso\":" + String(isoPSI,1) +
                  ",\"resin\":" + String(resinPSI,1) +
                  ",\"isoLow\":" + String(isoLowPSI,1) +
                  ",\"resinLow\":" + String(resinLowPSI,1) +
                  ",\"airPiston\":" + String(airPSI,1) +
                  ",\"apAir\":" + String(apAirPSI,1);

    if (!isnan(isoTempF)) json += ",\"isoTemp\":" + String(isoTempF,1);
    if (!isnan(resinTempF)) json += ",\"resinTemp\":" + String(resinTempF,1);
    if (!isnan(isoLowTempF)) json += ",\"isoLowTemp\":" + String(isoLowTempF,1);
    if (!isnan(resinLowTempF)) json += ",\"resinLowTemp\":" + String(resinLowTempF,1);

    json += ",\"drumAir\":" + String(drumAirEnabled) +
            ",\"spray\":" + String(sprayEnabled);

    if (sprayInterlockActive) {
      String safe = lastInterlockReason;
      safe.replace("\"", "'");
      json += ",\"interlock\":\"" + safe + "\"";
    } else {
      json += ",\"interlock\":null";
    }
    json += "}";

    RigSerial.println(json);
    last = now;
  }

  // Temperature read every second
  if (now - lastTemp >= 1000) {
    tempSensors.requestTemperatures();
    if (isoTempAssigned)      isoTempF      = tempSensors.getTempF(isoTempAddr);
    if (resinTempAssigned)    resinTempF    = tempSensors.getTempF(resinTempAddr);
    if (isoLowTempAssigned)   isoLowTempF   = tempSensors.getTempF(isoLowTempAddr);
    if (resinLowTempAssigned) resinLowTempF = tempSensors.getTempF(resinLowTempAddr);
    lastTemp = now;
  }
}