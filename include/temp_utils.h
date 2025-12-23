#pragma once

#include <Arduino.h>

String addressToString(const uint8_t addr[8]);
bool   parseAddressString(const String& s, uint8_t addr[8]);
void   printDS18B20Addresses();
