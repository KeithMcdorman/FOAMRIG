#pragma once

// Firmware version (shown on web UI + HMI)
extern const char* FW_VERSION;

// ---------- Sensor pins ----------
#define SENSOR_A_PIN         35   // Iso (A side)
#define SENSOR_B_PIN         34   // Resin (B side)
#define SENSOR_AIRPISTON_PIN 32   // Primary air piston pressure
#define SENSOR_APAIR_PIN     33   // Fusion AP air purge pressure
#define SENSOR_ISO_LOW_PIN   36   // Iso low pressure
#define SENSOR_RESIN_LOW_PIN 39   // Resin low pressure

// ---------- Relays ----------
#define RELAY3_PIN 25      // Spray/Park
#define RELAY4_PIN 26      // Drum Pump Air
#define RELAY5_PIN 27      // Hose heat zone 1
#define RELAY6_PIN 14      // Hose heat zone 2
#define RELAY7_PIN 12
#define RELAY8_PIN 13

#define RELAY_SPRAY_PIN     RELAY3_PIN
#define RELAY_DRUM_AIR_PIN  RELAY4_PIN
#define RELAY_HOSE1_PIN     RELAY5_PIN
#define RELAY_HOSE2_PIN     RELAY6_PIN

// ---------- Hose-tip indicator LED ----------
#define HOSE_LED_PIN 23
#define HOSE_LED_LEDC_CH   6
#define HOSE_LED_LEDC_FREQ 5000
#define HOSE_LED_LEDC_BITS 8
#define HOSE_LED_MAX_DUTY  ((1 << HOSE_LED_LEDC_BITS) - 1)

// ---------- DS18B20 OneWire bus ----------
#define ONE_WIRE_BUS_PIN 18
