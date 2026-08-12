// Boot log buffer.
//
// USB descriptors have to be configured before TinyUSB starts, so the CDC
// console cannot exist during SD mount, config parsing and image creation.
// Everything logged before then is buffered and replayed once a host actually
// opens the port, which keeps the early boot path debuggable.
#pragma once

#include <Arduino.h>

// Installs the ESP-IDF log hook so log_i()/log_e() are captured too.
void logInit();

void logPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOGF(...) logPrintf(__VA_ARGS__)

// Registers the USB CDC interface. Must run before USB.begin().
void logStartUsbSerial();

// Call from loop(): replays the buffer on first connect, then streams live.
void logPump();
