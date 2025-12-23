#pragma once

#include <Arduino.h>

// ADC -> PSI helpers
float readPSI_raw(int pin, float fullScalePsi);
float applyCalibration(float raw, float R0, float K, float fullScalePsi);

// Hose heat control + UI helpers
void applyHoseHeatControl();
const char* hoseStatusText(bool enabled, bool heating, float tempF, int setF, int swingF);
