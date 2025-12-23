#pragma once

#include <Arduino.h>
#include "rig_pins.h"

#ifndef ESP_ARDUINO_VERSION_MAJOR
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
static inline void hoseLedInit() {
  ledcAttach(HOSE_LED_PIN, HOSE_LED_LEDC_FREQ, HOSE_LED_LEDC_BITS);
  ledcWrite(HOSE_LED_PIN, 0);
}
static inline void hoseLedWrite(uint32_t duty) {
  ledcWrite(HOSE_LED_PIN, duty);
}
#else
static inline void hoseLedWrite(uint32_t duty) {
  ledcWrite(HOSE_LED_LEDC_CH, duty);
}
static inline void hoseLedInit() {
  ledcSetup(HOSE_LED_LEDC_CH, HOSE_LED_LEDC_FREQ, HOSE_LED_LEDC_BITS);
  ledcAttachPin(HOSE_LED_PIN, HOSE_LED_LEDC_CH);
  hoseLedWrite(0);
}
#endif
