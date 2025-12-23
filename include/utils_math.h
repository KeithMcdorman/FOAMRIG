#pragma once

// Small math helpers that must be visible across translation units.

static inline float round1(float v) {
  // Round to 1 decimal place
  return roundf(v * 10.0f) / 10.0f;
}
