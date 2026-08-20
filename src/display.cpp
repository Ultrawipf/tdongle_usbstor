#include "display.h"

#include <SPI.h>

#include "font5x7.h"
#include "pins.h"

#define LCD_SPI_HZ 20000000
#define LCD_LEDC_CHANNEL 3
#define LCD_LEDC_FREQ 1000
#define LCD_LEDC_BITS 8

// Character cell including the 1px inter-character gap.
#define CHAR_W 6
#define CHAR_H 8

static SPIClass sSpi(FSPI);
static bool sReady = false;
static bool sAsleep = false;
static uint8_t sBacklight = 0;  // level requested while awake

// ---------------------------------------------------------------------------
// low level
// ---------------------------------------------------------------------------

static inline void lcdSelect() { digitalWrite(LCD_CS_PIN, LOW); }
static inline void lcdDeselect() { digitalWrite(LCD_CS_PIN, HIGH); }

static void lcdCmd(uint8_t cmd) {
  digitalWrite(LCD_DC_PIN, LOW);
  lcdSelect();
  sSpi.write(cmd);
  lcdDeselect();
}

static void lcdData(const uint8_t *data, size_t len) {
  if (!len) return;
  digitalWrite(LCD_DC_PIN, HIGH);
  lcdSelect();
  sSpi.writeBytes(data, len);
  lcdDeselect();
}

static void lcdCmdData(uint8_t cmd, const uint8_t *data, size_t len) {
  lcdCmd(cmd);
  lcdData(data, len);
}

static void lcdSetWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
  uint16_t x0 = x + LCD_X_GAP;
  uint16_t x1 = x + w - 1 + LCD_X_GAP;
  uint16_t y0 = y + LCD_Y_GAP;
  uint16_t y1 = y + h - 1 + LCD_Y_GAP;

  uint8_t buf[4];
  buf[0] = x0 >> 8; buf[1] = x0 & 0xFF; buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
  lcdCmdData(0x2A, buf, 4);  // CASET
  buf[0] = y0 >> 8; buf[1] = y0 & 0xFF; buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
  lcdCmdData(0x2B, buf, 4);  // RASET
  lcdCmd(0x2C);              // RAMWR
}

// Pushes `count` pixels of one colour without re-issuing the window.
static void lcdPushColor(uint16_t color, uint32_t count) {
  uint8_t line[64];
  for (int i = 0; i < 32; i++) {
    line[i * 2] = color >> 8;
    line[i * 2 + 1] = color & 0xFF;
  }
  digitalWrite(LCD_DC_PIN, HIGH);
  lcdSelect();
  while (count) {
    uint32_t chunk = count > 32 ? 32 : count;
    sSpi.writeBytes(line, chunk * 2);
    count -= chunk;
  }
  lcdDeselect();
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

struct InitCmd {
  uint8_t cmd;
  uint8_t len;      // number of data bytes
  uint8_t delayMs;  // post-command delay
  uint8_t data[16];
};

static const InitCmd ST7735_INIT[] = {
    {0x01, 0, 150, {}},                                   // SWRESET
    {0x11, 0, 255, {}},                                   // SLPOUT
    {0xB1, 3, 0, {0x01, 0x2C, 0x2D}},                     // FRMCTR1
    {0xB2, 3, 0, {0x01, 0x2C, 0x2D}},                     // FRMCTR2
    {0xB3, 6, 0, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}},   // FRMCTR3
    {0xB4, 1, 0, {0x07}},                                 // INVCTR
    {0xC0, 3, 0, {0xA2, 0x02, 0x84}},                     // PWCTR1
    {0xC1, 1, 0, {0xC5}},                                 // PWCTR2
    {0xC2, 2, 0, {0x0A, 0x00}},                           // PWCTR3
    {0xC3, 2, 0, {0x8A, 0x2A}},                           // PWCTR4
    {0xC4, 2, 0, {0x8A, 0xEE}},                           // PWCTR5
    {0xC5, 1, 0, {0x0E}},                                 // VMCTR1
    {0x21, 0, 0, {}},                                     // INVON
    // MADCTL: MY|MV -> landscape with the vendor's mirror(false,true) +
    // swap_xy(true); BGR because the panel is wired BGR.
    {0x36, 1, 0, {0xA8}},
    {0x3A, 1, 0, {0x05}},                                 // COLMOD 16bpp
    {0xE0, 16, 0, {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25,
                   0x2B, 0x39, 0x00, 0x01, 0x03, 0x10}},  // GMCTRP1
    {0xE1, 16, 0, {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E,
                   0x37, 0x3F, 0x00, 0x00, 0x02, 0x10}},  // GMCTRN1
    {0x13, 0, 10, {}},                                    // NORON
    {0x29, 0, 100, {}},                                   // DISPON
};

// Writes the PWM duty for `level` (0..255), attaching the LEDC channel on the
// first call. Backlight is active low, so full brightness is duty 0.
static void backlightDuty(uint8_t level) {
  static bool attached = false;
  if (!attached) {
    ledcSetup(LCD_LEDC_CHANNEL, LCD_LEDC_FREQ, LCD_LEDC_BITS);
    ledcAttachPin(LCD_BL_PIN, LCD_LEDC_CHANNEL);
    attached = true;
  }
  uint32_t duty = LCD_BL_ACTIVE_LEVEL == 0 ? (255 - level) : level;
  ledcWrite(LCD_LEDC_CHANNEL, duty);
}

bool displayBegin(uint8_t backlight) {
  pinMode(LCD_CS_PIN, OUTPUT);
  pinMode(LCD_DC_PIN, OUTPUT);
  pinMode(LCD_RST_PIN, OUTPUT);
  lcdDeselect();

  digitalWrite(LCD_RST_PIN, HIGH);
  delay(10);
  digitalWrite(LCD_RST_PIN, LOW);
  delay(20);
  digitalWrite(LCD_RST_PIN, HIGH);
  delay(150);

  sSpi.begin(LCD_SCLK_PIN, -1, LCD_MOSI_PIN, -1);
  sSpi.beginTransaction(SPISettings(LCD_SPI_HZ, MSBFIRST, SPI_MODE0));

  for (size_t i = 0; i < sizeof(ST7735_INIT) / sizeof(ST7735_INIT[0]); i++) {
    const InitCmd &c = ST7735_INIT[i];
    lcdCmdData(c.cmd, c.data, c.len);
    if (c.delayMs) delay(c.delayMs);
  }

  sReady = true;
  displayFill(COLOR_BLACK);
  displaySetBacklight(backlight);
  return true;
}

void displayEnd() {
  if (!sReady) return;
  backlightDuty(0);
  lcdCmd(0x28);  // DISPOFF
  sReady = false;
  sAsleep = false;
}

bool displayAvailable() { return sReady; }

void displaySetBacklight(uint8_t level) {
  sBacklight = level;
  // While asleep the level is only remembered; waking applies it.
  if (!sAsleep) backlightDuty(level);
}

// Idle sleep. Unlike displayEnd() this keeps the panel initialised, so waking
// is a single DISPON instead of the ~300 ms reset dance in displayBegin().
void displaySleep() {
  if (!sReady || sAsleep) return;
  backlightDuty(0);
  lcdCmd(0x28);  // DISPOFF
  sAsleep = true;
}

void displayWake() {
  if (!sReady || !sAsleep) return;
  sAsleep = false;
  lcdCmd(0x29);  // DISPON
  backlightDuty(sBacklight);
  // The panel kept its framebuffer, but the caller may have skipped updates
  // while it was off, so force a full repaint.
  displayInvalidate();
}

bool displayAsleep() { return sAsleep; }

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

void displayFill(uint16_t color) {
  displayFillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

void displayFillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                     uint16_t color) {
  if (!sReady) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
  if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
  if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
  if (w <= 0 || h <= 0) return;

  lcdSetWindow(x, y, w, h);
  lcdPushColor(color, (uint32_t)w * h);
}

// Renders one character into a pixel buffer and blits it in a single window.
static void drawChar(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg,
                     uint8_t scale) {
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;

  const uint8_t *glyph = nullptr;
  if (c >= FONT_FIRST_CHAR && c <= FONT_LAST_CHAR) {
    glyph = FONT5X7[(uint8_t)c - FONT_FIRST_CHAR];
  }

  int16_t w = CHAR_W * scale;
  int16_t h = CHAR_H * scale;
  if (x + w > LCD_WIDTH || y + h > LCD_HEIGHT) {
    // Partially off-screen: fall back to a background fill rather than
    // clipping the glyph, which keeps the buffer logic simple.
    displayFillRect(x, y, w, h, bg);
    return;
  }

  // Max cell is 6*3 x 8*3 = 18x24 px = 864 bytes; scale is capped below.
  static uint8_t buf[6 * 3 * 8 * 3 * 2];
  size_t idx = 0;
  for (int16_t row = 0; row < h; row++) {
    int fontRow = row / scale;
    for (int16_t col = 0; col < w; col++) {
      int fontCol = col / scale;
      bool on = false;
      if (glyph && fontCol < FONT_WIDTH && fontRow < FONT_HEIGHT) {
        on = (glyph[fontCol] >> fontRow) & 1;
      }
      uint16_t color = on ? fg : bg;
      buf[idx++] = color >> 8;
      buf[idx++] = color & 0xFF;
    }
  }

  lcdSetWindow(x, y, w, h);
  digitalWrite(LCD_DC_PIN, HIGH);
  lcdSelect();
  sSpi.writeBytes(buf, idx);
  lcdDeselect();
}

void displayText(int16_t x, int16_t y, const char *text, uint16_t fg,
                 uint16_t bg, uint8_t scale) {
  if (!sReady || !text) return;
  if (scale < 1) scale = 1;
  if (scale > 3) scale = 3;
  int16_t cx = x;
  for (const char *p = text; *p; p++) {
    drawChar(cx, y, *p, fg, bg, scale);
    cx += CHAR_W * scale;
    if (cx >= LCD_WIDTH) break;
  }
}

int16_t displayTextClipped(int16_t x, int16_t y, const char *text, uint16_t fg,
                           uint16_t bg, uint8_t scale, int16_t maxWidth) {
  if (!sReady || !text) return 0;
  if (scale < 1) scale = 1;
  if (scale > 3) scale = 3;

  int16_t cellW = CHAR_W * scale;
  int16_t maxChars = maxWidth / cellW;
  if (maxChars <= 0) return 0;

  int len = strlen(text);
  char line[48];
  if (len <= maxChars) {
    snprintf(line, sizeof(line), "%s", text);
  } else if (maxChars <= 3) {
    snprintf(line, sizeof(line), "%.*s", maxChars, text);
  } else {
    // Keep the tail: image names differ more at the end than at the start.
    int keep = maxChars - 3;
    snprintf(line, sizeof(line), "...%s", text + (len - keep));
  }

  displayText(x, y, line, fg, bg, scale);
  int16_t used = strlen(line) * cellW;
  if (used < maxWidth) {
    displayFillRect(x + used, y, maxWidth - used, CHAR_H * scale, bg);
  }
  return used;
}

// ---------------------------------------------------------------------------
// screens
// ---------------------------------------------------------------------------

static void formatSize(uint64_t bytes, char *out, size_t outLen) {
  if (bytes >= (1ULL << 30)) {
    snprintf(out, outLen, "%.1f GB", (double)bytes / (double)(1ULL << 30));
  } else if (bytes >= (1ULL << 20)) {
    snprintf(out, outLen, "%llu MB", (unsigned long long)(bytes >> 20));
  } else {
    snprintf(out, outLen, "%llu KB", (unsigned long long)(bytes >> 10));
  }
}

void displayMessage(const char *title, const char *line1, const char *line2,
                    uint16_t accent) {
  if (!sReady) return;
  displayFill(COLOR_BLACK);
  displayFillRect(0, 0, LCD_WIDTH, 14, accent);
  displayTextClipped(3, 3, title, COLOR_BLACK, accent, 1, LCD_WIDTH - 6);
  if (line1) displayTextClipped(3, 26, line1, COLOR_WHITE, COLOR_BLACK, 2, LCD_WIDTH - 6);
  if (line2) displayTextClipped(3, 56, line2, COLOR_GREY, COLOR_BLACK, 1, LCD_WIDTH - 6);
  displayInvalidate();
}

void displaySelect(const String &name, size_t index, size_t total,
                   uint64_t sizeBytes) {
  if (!sReady) return;

  char header[32];
  snprintf(header, sizeof(header), "SELECT IMAGE  %u/%u", (unsigned)(index + 1),
           (unsigned)total);

  char size[24];
  formatSize(sizeBytes, size, sizeof(size));

  char footer[40];
  snprintf(footer, sizeof(footer), "%s - hold to confirm", size);

  displayFill(COLOR_BLACK);
  displayFillRect(0, 0, LCD_WIDTH, 14, COLOR_CYAN);
  displayText(3, 3, header, COLOR_BLACK, COLOR_CYAN, 1);
  displayTextClipped(3, 28, name.c_str(), COLOR_WHITE, COLOR_BLACK, 2,
                     LCD_WIDTH - 6);
  displayTextClipped(3, 60, footer, COLOR_GREY, COLOR_BLACK, 1, LCD_WIDTH - 6);
  displayInvalidate();
}

// Cached state so displayStatus() only repaints what actually changed.
static String sLastTitle;
static String sLastImage;
static uint64_t sLastSize = 0;
static int sLastReadOnly = -1;
static int sLastMounted = -1;
static int sLastActivity = -1;

void displayInvalidate() {
  sLastTitle = "";
  sLastImage = "";
  sLastSize = 0;
  sLastReadOnly = -1;
  sLastMounted = -1;
  sLastActivity = -1;
}

void displayStatus(const String &title, const String &imageName,
                   uint64_t sizeBytes, bool readOnly, bool hostMounted,
                   bool reading, bool writing) {
  if (!sReady) return;

  if (title != sLastTitle) {
    displayFillRect(0, 0, LCD_WIDTH, 14, COLOR_DARK);
    displayTextClipped(3, 3, title.c_str(), COLOR_CYAN, COLOR_DARK, 1,
                       LCD_WIDTH - 6);
    sLastTitle = title;
  }

  if (imageName != sLastImage) {
    displayTextClipped(3, 22, imageName.c_str(), COLOR_WHITE, COLOR_BLACK, 2,
                       LCD_WIDTH - 6);
    sLastImage = imageName;
  }

  int roFlag = readOnly ? 1 : 0;
  if (sizeBytes != sLastSize || roFlag != sLastReadOnly) {
    char size[24];
    formatSize(sizeBytes, size, sizeof(size));
    char line[40];
    snprintf(line, sizeof(line), "%s  %s", size, readOnly ? "READ-ONLY" : "READ/WRITE");
    displayTextClipped(3, 46, line, readOnly ? COLOR_YELLOW : COLOR_GREEN,
                       COLOR_BLACK, 1, LCD_WIDTH - 6);
    sLastSize = sizeBytes;
    sLastReadOnly = roFlag;
  }

  int mountedFlag = hostMounted ? 1 : 0;
  int activity = writing ? 2 : (reading ? 1 : 0);
  if (mountedFlag != sLastMounted || activity != sLastActivity) {
    const char *state;
    uint16_t color;
    if (writing) {
      state = "WRITING";
      color = COLOR_MAGENTA;
    } else if (reading) {
      state = "READING";
      color = COLOR_BLUE;
    } else if (hostMounted) {
      state = "CONNECTED";
      color = COLOR_GREEN;
    } else {
      state = "WAITING FOR HOST";
      color = COLOR_GREY;
    }
    displayFillRect(0, 64, LCD_WIDTH, 16, COLOR_BLACK);
    displayText(3, 66, state, color, COLOR_BLACK, 1);
    // Activity dot on the right edge.
    displayFillRect(LCD_WIDTH - 12, 66, 8, 8, color);
    sLastMounted = mountedFlag;
    sLastActivity = activity;
  }
}
