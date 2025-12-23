#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include "rig_globals.h"

void setupWiFi() {
  WiFi.persistent(false);
  WiFi.disconnect(true, false);
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

