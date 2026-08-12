// TinyUSB mass storage device with configurable descriptors.
#pragma once

#include <Arduino.h>

#include "config.h"

// Maximum length of a USB string descriptor (manufacturer / product / serial).
//
// A string descriptor's bLength is a single byte and the payload is UTF-16, so
// the protocol ceiling is (255 - 2) / 2 = 126 characters. The Arduino core
// copies through snprintf(dst, 126, ...) which costs one more, leaving 125.
// Longer values are truncated with a warning rather than silently cut.
#define USB_STRING_MAX_CHARS 125

// Applies the USB descriptors from `cfg`, wires up the MSC callbacks against
// the currently open image and starts TinyUSB. Must be called after
// storageOpenImage(). Returns false if no image is open.
bool usbMscBegin(const AppConfig &cfg);

// True once the host has enumerated and not suspended the device.
bool usbMscHostConnected();

// The serial string actually used (config value, or the MAC-derived default).
const String &usbMscSerial();
