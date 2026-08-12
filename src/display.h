// Minimal ST7735 driver + status screens for the 160x80 panel.
//
// Deliberately dependency-free: the UI is a handful of text lines and a couple
// of filled rectangles, which is far less code than configuring TFT_eSPI or
// pulling in Adafruit_GFX.
#pragma once

#include <Arduino.h>

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F
#define COLOR_GREY    0x8410
#define COLOR_DARK    0x18E3

bool displayBegin(uint8_t backlight);
void displayEnd();
bool displayAvailable();
void displaySetBacklight(uint8_t level);

void displayFill(uint16_t color);
void displayFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
// Draws `text` at (x,y). Characters are 6*scale wide and 8*scale tall.
void displayText(int16_t x, int16_t y, const char *text, uint16_t fg,
                 uint16_t bg, uint8_t scale);
// Same, but clipped to `maxWidth` pixels with a trailing ellipsis if it does
// not fit. Returns the width actually used.
int16_t displayTextClipped(int16_t x, int16_t y, const char *text, uint16_t fg,
                           uint16_t bg, uint8_t scale, int16_t maxWidth);

// ---- high level screens -------------------------------------------------

// Boot/error banner.
void displayMessage(const char *title, const char *line1, const char *line2,
                    uint16_t accent);

// Image selection screen shown while the button is used to cycle images.
void displaySelect(const String &name, size_t index, size_t total,
                   uint64_t sizeBytes);

// Main status screen. Redraws only what changed, so it is cheap to call often.
void displayStatus(const String &title, const String &imageName,
                   uint64_t sizeBytes, bool readOnly, bool hostMounted,
                   bool reading, bool writing);

// Forces the next displayStatus() call to redraw every element.
void displayInvalidate();
