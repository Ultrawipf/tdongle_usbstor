// TinyUSB mass storage device with configurable descriptors.
#pragma once

#include <Arduino.h>

#include "config.h"

// Applies the USB descriptors from `cfg`, wires up the MSC callbacks against
// the currently open image and starts TinyUSB. Must be called after
// storageOpenImage(). Returns false if no image is open.
bool usbMscBegin(const AppConfig &cfg);

// True once the host has enumerated and not suspended the device.
bool usbMscHostConnected();

// The serial string actually used (config value, or the MAC-derived default).
const String &usbMscSerial();
