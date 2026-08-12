#include "status_led.h"

#include "pins.h"

#define ACCESS_BLINK_MS 60

static uint8_t sBrightness = 8;
static bool sEnabled = true;
static LedState sState = LED_IDLE;
static uint32_t sLastSent = 0xFFFFFFFF;

static inline void shiftByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(LED_DI_PIN, (b >> i) & 1);
    digitalWrite(LED_CI_PIN, HIGH);
    digitalWrite(LED_CI_PIN, LOW);
  }
}

// APA102 frame: 4 zero bytes, one LED frame, then >= n/2 bits of end frame.
static void sendColor(uint8_t r, uint8_t g, uint8_t b) {
  shiftByte(0x00);
  shiftByte(0x00);
  shiftByte(0x00);
  shiftByte(0x00);

  shiftByte(0xE0 | (sBrightness & 0x1F));
  shiftByte(b);
  shiftByte(g);
  shiftByte(r);

  shiftByte(0xFF);
}

void ledBegin(uint8_t brightness) {
  sBrightness = brightness > 31 ? 31 : brightness;
  pinMode(LED_DI_PIN, OUTPUT);
  pinMode(LED_CI_PIN, OUTPUT);
  digitalWrite(LED_DI_PIN, LOW);
  digitalWrite(LED_CI_PIN, LOW);
  sLastSent = 0xFFFFFFFF;
  sendColor(0, 0, 0);
}

void ledSetEnabled(bool enabled) {
  sEnabled = enabled;
  if (!enabled) ledOff();
}

void ledSetState(LedState state) { sState = state; }

static void baseColor(uint8_t *r, uint8_t *g, uint8_t *b) {
  switch (sState) {
    case LED_ERROR:  *r = 255; *g = 0;   *b = 0;   break;
    case LED_IDLE:   *r = 255; *g = 90;  *b = 0;   break;
    case LED_READY:  *r = 0;   *g = 255; *b = 0;   break;
    case LED_SELECT: *r = 0;   *g = 180; *b = 255; break;
    case LED_MAINT:  *r = 255; *g = 255; *b = 255; break;
    default:         *r = 0;   *g = 0;   *b = 0;   break;
  }
}

void ledUpdate(uint32_t lastReadMs, uint32_t lastWriteMs) {
  if (!sEnabled) return;

  uint8_t r, g, b;
  baseColor(&r, &g, &b);

  uint32_t now = millis();
  // Writes win over reads so a write burst is clearly visible.
  if (lastWriteMs && (uint32_t)(now - lastWriteMs) < ACCESS_BLINK_MS) {
    r = 255; g = 0; b = 255;  // magenta
  } else if (lastReadMs && (uint32_t)(now - lastReadMs) < ACCESS_BLINK_MS) {
    r = 0; g = 80; b = 255;   // blue
  }

  uint32_t packed = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (packed == sLastSent) return;  // avoid pointless bit-banging every loop
  sLastSent = packed;
  sendColor(r, g, b);
}

void ledOff() {
  sLastSent = 0;
  sendColor(0, 0, 0);
}
