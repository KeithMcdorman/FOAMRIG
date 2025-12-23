#pragma once

#include <Arduino.h>

// Shared helper for building interlock message text.
String makeLowSupplyInterlockReason(float isoHP, float resinHP,
                                   float isoLow, float resinLow,
                                   int supplyLowPSI);

// Interlock reset helpers (implemented in web_handlers.cpp)
bool hoseOvertempConditionCleared();
bool lowSupplyConditionCleared();
void handleInterlockReset();
