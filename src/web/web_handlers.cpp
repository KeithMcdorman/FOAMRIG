// Web route handlers (split from original monolithic main.cpp)

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <DallasTemperature.h>

#include "rig_pins.h"
#include "rig_globals.h"
#include "sensors.h"
#include "temp_utils.h"
#include "utils_math.h"
#include "interlock.h"

// Page blobs live in web_pages.cpp
extern const char* mainPage;
extern const char* settingsPage;
extern const char* updatePage;

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
    // IMPORTANT: only update fields that are actually present in the JSON.
    // This prevents accidental overwrites to zero when the UI submits blank fields.
    if (doc.containsKey("target") && doc["target"].is<int>())         targetPressure = doc["target"].as<int>();
    if (doc.containsKey("margin") && doc["margin"].is<int>())         marginPercent  = doc["margin"].as<int>();
    if (doc.containsKey("diff") && doc["diff"].is<int>())             diffPressure   = doc["diff"].as<int>();
    if (doc.containsKey("airTarget") && doc["airTarget"].is<int>())   airTarget      = doc["airTarget"].as<int>();
    if (doc.containsKey("gunTarget") && doc["gunTarget"].is<int>())   gunTarget      = doc["gunTarget"].as<int>();
    if (doc.containsKey("isoLowTarget") && doc["isoLowTarget"].is<int>())     isoLowTarget   = doc["isoLowTarget"].as<int>();
    if (doc.containsKey("resinLowTarget") && doc["resinLowTarget"].is<int>()) resinLowTarget = doc["resinLowTarget"].as<int>();
    if (doc.containsKey("supplyLow") && doc["supplyLow"].is<int>())   supplyLowPSI   = doc["supplyLow"].as<int>();

    if (doc.containsKey("isoTempTarget") && doc["isoTempTarget"].is<int>())         isoTempTargetF      = doc["isoTempTarget"].as<int>();
    if (doc.containsKey("resinTempTarget") && doc["resinTempTarget"].is<int>())     resinTempTargetF    = doc["resinTempTarget"].as<int>();
    if (doc.containsKey("isoLowTempTarget") && doc["isoLowTempTarget"].is<int>())   isoLowTempTargetF   = doc["isoLowTempTarget"].as<int>();
    if (doc.containsKey("resinLowTempTarget") && doc["resinLowTempTarget"].is<int>()) resinLowTempTargetF = doc["resinLowTempTarget"].as<int>();
    if (doc.containsKey("tempMinF") && doc["tempMinF"].is<int>())                   tempMinF            = doc["tempMinF"].as<int>();
    if (doc.containsKey("tempMaxF") && doc["tempMaxF"].is<int>())                   tempMaxF            = doc["tempMaxF"].as<int>();

    // Hose heat settings
    if (doc.containsKey("hose1Set") && doc["hose1Set"].is<int>()) hose1SetF = doc["hose1Set"].as<int>();
    if (doc.containsKey("hose2Set") && doc["hose2Set"].is<int>()) hose2SetF = doc["hose2Set"].as<int>();
    if (doc.containsKey("hose1Tol") && doc["hose1Tol"].is<int>()) hose1TolF = doc["hose1Tol"].as<int>();
    if (doc.containsKey("hose2Tol") && doc["hose2Tol"].is<int>()) hose2TolF = doc["hose2Tol"].as<int>();
    if (doc.containsKey("hose1En")  && doc["hose1En"].is<bool>())  hose1Enabled = doc["hose1En"].as<bool>();
    if (doc.containsKey("hose2En")  && doc["hose2En"].is<bool>())  hose2Enabled = doc["hose2En"].as<bool>();
    if (doc.containsKey("hoseOvertempF") && doc["hoseOvertempF"].is<int>()) hoseOvertempF = doc["hoseOvertempF"].as<int>();

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

    if (doc.containsKey("wifiMode") && doc["wifiMode"].is<int>()) {
      networkMode = (int)doc["wifiMode"];
      if (networkMode != NETMODE_AP && networkMode != NETMODE_STA) {
        networkMode = NETMODE_AP;
      }
    }
    // Wi-Fi strings: only update if present AND non-empty, to avoid accidental blank saves.
    if (doc.containsKey("apSsid") && doc["apSsid"].is<const char*>()) {
      const char* v = doc["apSsid"];
      if (v && v[0] != '\0') apSsid = String(v);
    }
    if (doc.containsKey("apPass") && doc["apPass"].is<const char*>()) {
      const char* v = doc["apPass"];
      if (v && v[0] != '\0') apPass = String(v);
    }
    if (doc.containsKey("staSsid") && doc["staSsid"].is<const char*>()) {
      const char* v = doc["staSsid"];
      if (v && v[0] != '\0') staSsid = String(v);
    }
    if (doc.containsKey("staPass") && doc["staPass"].is<const char*>()) {
      const char* v = doc["staPass"];
      if (v && v[0] != '\0') staPass = String(v);
    }

    // Manual overrides
    if (doc.containsKey("forceDrumAir") && doc["forceDrumAir"].is<bool>()) forceDrumAirOverride = doc["forceDrumAir"].as<bool>();
    if (doc.containsKey("forceSpray")   && doc["forceSpray"].is<bool>())   forceSprayOverride   = doc["forceSpray"].as<bool>();

    // Defensive defaults: prevent saving invalid AP credentials that would break softAP().
    if (apSsid.length() == 0) apSsid = DEFAULT_AP_SSID;
    if (apPass.length() == 0) apPass = DEFAULT_AP_PASS;

    // Persist
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

    prefs.putBool("forceDrumAir", forceDrumAirOverride);
    prefs.putBool("forceSpray",   forceSprayOverride);

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

// Create a backup snapshot of the current persisted settings.
// This is intentionally simple: it stores a parallel set of *_bak keys in NVS.
void handleSettingsBackup() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  // Core pressure/ratio
  prefs.putInt("target_bak",         targetPressure);
  prefs.putInt("margin_bak",         marginPercent);
  prefs.putInt("diff_bak",           diffPressure);
  prefs.putInt("airTarget_bak",      airTarget);
  prefs.putInt("gunTarget_bak",      gunTarget);
  prefs.putInt("isoLowTarget_bak",   isoLowTarget);
  prefs.putInt("resinLowTarget_bak", resinLowTarget);
  prefs.putInt("supplyLow_bak",      supplyLowPSI);

  // Temps
  prefs.putInt("isoTempTarget_bak",  isoTempTargetF);
  prefs.putInt("resTempTarget_bak",  resinTempTargetF);
  prefs.putInt("isoLowTempTgt_bak",  isoLowTempTargetF);
  prefs.putInt("resLowTempTgt_bak",  resinLowTempTargetF);
  prefs.putInt("tempMinF_bak",       tempMinF);
  prefs.putInt("tempMaxF_bak",       tempMaxF);

  // Hose heat
  prefs.putBool("hose1En_bak",       hose1Enabled);
  prefs.putBool("hose2En_bak",       hose2Enabled);
  prefs.putInt("hose1SetF_bak",      hose1SetF);
  prefs.putInt("hose2SetF_bak",      hose2SetF);
  prefs.putInt("hose1TolF_bak",      hose1TolF);
  prefs.putInt("hose2TolF_bak",      hose2TolF);
  prefs.putInt("hoseOvertempF_bak",  hoseOvertempF);

  // Wi-Fi
  prefs.putInt("wifiMode_bak",       networkMode);
  prefs.putString("apSsid_bak",      apSsid);
  prefs.putString("apPass_bak",      apPass);
  prefs.putString("staSsid_bak",     staSsid);
  prefs.putString("staPass_bak",     staPass);

  // Manual overrides
  prefs.putBool("forceDrumAir_bak",  forceDrumAirOverride);
  prefs.putBool("forceSpray_bak",    forceSprayOverride);

  server.send(200, "text/plain", "OK");
}

// Restore settings from the last backup snapshot.
void handleSettingsRestore() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  // Pull from backup keys, falling back to current runtime values if missing.
  targetPressure      = prefs.getInt("target_bak",         targetPressure);
  marginPercent       = prefs.getInt("margin_bak",         marginPercent);
  diffPressure        = prefs.getInt("diff_bak",           diffPressure);
  airTarget           = prefs.getInt("airTarget_bak",      airTarget);
  gunTarget           = prefs.getInt("gunTarget_bak",      gunTarget);
  isoLowTarget        = prefs.getInt("isoLowTarget_bak",   isoLowTarget);
  resinLowTarget      = prefs.getInt("resinLowTarget_bak", resinLowTarget);
  supplyLowPSI        = prefs.getInt("supplyLow_bak",      supplyLowPSI);

  isoTempTargetF      = prefs.getInt("isoTempTarget_bak",  isoTempTargetF);
  resinTempTargetF    = prefs.getInt("resTempTarget_bak",  resinTempTargetF);
  isoLowTempTargetF   = prefs.getInt("isoLowTempTgt_bak",  isoLowTempTargetF);
  resinLowTempTargetF = prefs.getInt("resLowTempTgt_bak",  resinLowTempTargetF);
  tempMinF            = prefs.getInt("tempMinF_bak",       tempMinF);
  tempMaxF            = prefs.getInt("tempMaxF_bak",       tempMaxF);

  hose1Enabled        = prefs.getBool("hose1En_bak",       hose1Enabled);
  hose2Enabled        = prefs.getBool("hose2En_bak",       hose2Enabled);
  hose1SetF           = prefs.getInt("hose1SetF_bak",      hose1SetF);
  hose2SetF           = prefs.getInt("hose2SetF_bak",      hose2SetF);
  hose1TolF           = prefs.getInt("hose1TolF_bak",      hose1TolF);
  hose2TolF           = prefs.getInt("hose2TolF_bak",      hose2TolF);
  hoseOvertempF       = prefs.getInt("hoseOvertempF_bak",  hoseOvertempF);

  networkMode         = prefs.getInt("wifiMode_bak",       networkMode);
  apSsid              = prefs.getString("apSsid_bak",      apSsid);
  apPass              = prefs.getString("apPass_bak",      apPass);
  staSsid             = prefs.getString("staSsid_bak",     staSsid);
  staPass             = prefs.getString("staPass_bak",     staPass);

  forceDrumAirOverride = prefs.getBool("forceDrumAir_bak", forceDrumAirOverride);
  forceSprayOverride   = prefs.getBool("forceSpray_bak",   forceSprayOverride);

  // Defensive defaults for AP credentials
  if (apSsid.length() == 0) apSsid = DEFAULT_AP_SSID;
  if (apPass.length() == 0) apPass = DEFAULT_AP_PASS;

  // Re-persist the restored values to the primary keys so they survive reboot.
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

  prefs.putBool("hose1En",       hose1Enabled);
  prefs.putBool("hose2En",       hose2Enabled);
  prefs.putInt("hose1SetF",      hose1SetF);
  prefs.putInt("hose2SetF",      hose2SetF);
  prefs.putInt("hose1TolF",      hose1TolF);
  prefs.putInt("hose2TolF",      hose2TolF);
  prefs.putInt("hoseOvertempF",  hoseOvertempF);

  prefs.putInt("wifiMode",       networkMode);
  prefs.putString("apSsid",      apSsid);
  prefs.putString("apPass",      apPass);
  prefs.putString("staSsid",     staSsid);
  prefs.putString("staPass",     staPass);

  prefs.putBool("forceDrumAir",  forceDrumAirOverride);
  prefs.putBool("forceSpray",    forceSprayOverride);

  server.send(200, "text/plain", "OK");
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
bool hoseOvertempConditionCleared()
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

bool lowSupplyConditionCleared()
{
  return (lastIsoLowPSI >= supplyLowPSI) && (lastResinLowPSI >= supplyLowPSI);
}

void handleInterlockReset()
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

