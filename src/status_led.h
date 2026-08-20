// Single APA102 status LED (bit-banged; one LED does not justify a library).
#pragma once

#include <Arduino.h>

enum LedState {
  LED_ERROR,      // red    - no SD card / no image
  LED_IDLE,       // amber  - ready, USB not mounted by a host
  LED_READY,      // green  - host has the device mounted
  LED_SELECT,     // cyan   - image selection mode
  LED_MAINT,      // white  - maintenance mode, raw card exposed
};

void ledBegin(uint8_t brightness);  // brightness 0..31 (APA102 global current)
void ledSetEnabled(bool enabled);
// When enabled, the steady LED_READY colour is suppressed: the LED stays dark
// while the host has the volume mounted and nothing is being accessed. Access
// blinks and every other state are unaffected.
void ledSetIdleOff(bool enabled);
void ledSetState(LedState state);

// Call regularly from loop(); overlays short read/write access blinks on top of
// the base state.
void ledUpdate(uint32_t lastReadMs, uint32_t lastWriteMs);

void ledOff();
