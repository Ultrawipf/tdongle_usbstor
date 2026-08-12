#include "logbuf.h"

#include <USBCDC.h>
#include <esp_log.h>
#include <stdarg.h>

#define LOG_BUFFER_SIZE 4096

static char sBuf[LOG_BUFFER_SIZE];
static size_t sLen = 0;
static bool sTruncated = false;
static bool sReplayed = false;

static USBCDC sUsbSerial;
static bool sCdcStarted = false;

static void logAppend(const char *data, size_t n) {
  if (sReplayed && sCdcStarted) {
    sUsbSerial.write((const uint8_t *)data, n);
    return;
  }
  if (sLen + n > LOG_BUFFER_SIZE) {
    n = LOG_BUFFER_SIZE - sLen;
    sTruncated = true;
  }
  if (!n) return;
  memcpy(sBuf + sLen, data, n);
  sLen += n;
}

static int logVprintf(const char *fmt, va_list args) {
  char line[256];
  int n = vsnprintf(line, sizeof(line), fmt, args);
  if (n > 0) {
    logAppend(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
  }
  return n;
}

void logInit() {
  // Route ESP-IDF logging (log_i/log_e in the other modules) through the same
  // buffer so nothing from the boot path is lost.
  esp_log_set_vprintf(logVprintf);
}

void logPrintf(const char *fmt, ...) {
  char line[256];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (n > 0) {
    logAppend(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
  }
}

void logStartUsbSerial() {
  if (sCdcStarted) return;
  sUsbSerial.begin();
  sCdcStarted = true;
}

void logPump() {
  if (!sCdcStarted || sReplayed) return;
  if (!sUsbSerial) return;  // host has not opened the port yet

  sUsbSerial.write((const uint8_t *)sBuf, sLen);
  if (sTruncated) {
    sUsbSerial.print("\n[log] buffer overflowed, earlier lines dropped\n");
  }
  sUsbSerial.flush();
  sReplayed = true;
  sLen = 0;
}
