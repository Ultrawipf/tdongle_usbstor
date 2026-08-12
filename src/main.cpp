// usb_stor_emu - LilyGO T-Dongle-S3 as a configurable USB mass storage device.
//
// Flow:
//   SD mount -> read /usbstor.ini -> pick an image from the image directory
//   (button-selectable) -> apply per-image USB overrides -> start TinyUSB MSC.
//
// The MSC device serves the raw sectors of the selected image file, so the host
// owns the filesystem inside it and reads/writes behave exactly like a real
// stick. Only the selected image is visible; nothing else on the card is.

#include <Arduino.h>
#include <SD_MMC.h>
#include <USB.h>
#include <esp32-hal-tinyusb.h>
#include <esp_system.h>

#include "config.h"
#include "display.h"
#include "logbuf.h"
#include "pins.h"
#include "provision.h"
#include "status_led.h"
#include "storage.h"
#include "usb_msc.h"

// Button timing
#define BTN_DEBOUNCE_MS 30
#define BTN_LONG_PRESS_MS 800
#define SELECT_IDLE_TIMEOUT_MS 15000

// How long after the last access we keep showing "reading"/"writing".
#define ACTIVITY_HOLD_MS 150

static AppConfig gConfig;
static String gImages[STORAGE_MAX_IMAGES];
static size_t gImageCount = 0;
static String gActiveImage;
static bool gRunning = false;
static bool gMaintenance = false;

// Survives a software restart but not a power cycle, which is exactly the
// lifetime maintenance mode should have.
#define BOOT_MODE_MAINTENANCE 0x4D41494EUL  // 'MAIN'
RTC_NOINIT_ATTR static uint32_t gBootMode;

// Reboots with the USB peripheral fully reset, so the host re-enumerates the
// device rather than reusing the previous descriptors and capacity.
static void restartInto(uint32_t mode) {
  gBootMode = mode;
  storageFlush();
  delay(150);
  usb_persist_restart(RESTART_NO_PERSIST);
  esp_restart();  // not reached, but keeps the no-return contract explicit
}

// ---------------------------------------------------------------------------
// button
// ---------------------------------------------------------------------------

static inline bool btnDown() { return digitalRead(BOOT_PIN) == LOW; }

// Blocks until the button is released; returns true if it was held long enough
// to count as a long press.
static bool btnWaitRelease() {
  uint32_t start = millis();
  while (btnDown()) {
    delay(10);
    if ((millis() - start) > 5000) break;  // stuck button guard
  }
  return (millis() - start) >= BTN_LONG_PRESS_MS;
}

// ---------------------------------------------------------------------------
// image selection
// ---------------------------------------------------------------------------

static uint64_t imageSizeOf(const String &name) {
  String path = gConfig.imageDir;
  if (!path.endsWith("/")) path += "/";
  path += name;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return 0;
  uint64_t s = f.size();
  f.close();
  return s;
}

static size_t indexOfImage(const String &name) {
  for (size_t i = 0; i < gImageCount; i++) {
    if (gImages[i].equalsIgnoreCase(name)) return i;
  }
  return 0;
}

// The picker cycles the images plus one extra virtual entry for maintenance
// mode, so a single gesture reaches everything.
#define SELECT_MAINTENANCE(count) (count)

static void showSelection(size_t idx, size_t total) {
  if (idx == SELECT_MAINTENANCE(gImageCount)) {
    displaySelect("MAINTENANCE", idx, total, 0);
    LOGF("[select] %u/%u <maintenance>\n", (unsigned)(idx + 1),
         (unsigned)total);
  } else {
    displaySelect(gImages[idx], idx, total, imageSizeOf(gImages[idx]));
    LOGF("[select] %u/%u %s\n", (unsigned)(idx + 1), (unsigned)total,
         gImages[idx].c_str());
  }
}

// Interactive picker: short press cycles, long press confirms, idle confirms.
// Returns the chosen index, which may be SELECT_MAINTENANCE(gImageCount).
static size_t runSelector(size_t startIndex) {
  size_t total = gImageCount + 1;  // + maintenance
  size_t idx = startIndex < total ? startIndex : 0;

  ledSetState(LED_SELECT);
  ledUpdate(0, 0);
  showSelection(idx, total);

  uint32_t lastInput = millis();

  // Wait out the press that got us here.
  while (btnDown()) delay(10);
  delay(BTN_DEBOUNCE_MS);

  while ((millis() - lastInput) < SELECT_IDLE_TIMEOUT_MS) {
    if (btnDown()) {
      delay(BTN_DEBOUNCE_MS);
      if (!btnDown()) continue;

      if (btnWaitRelease()) break;  // long press -> confirm

      idx = (idx + 1) % total;
      lastInput = millis();
      showSelection(idx, total);
      delay(BTN_DEBOUNCE_MS);
    }
    delay(10);
  }

  return idx;
}

// ---------------------------------------------------------------------------
// fatal error path
// ---------------------------------------------------------------------------

static void fail(const char *line1, const char *line2) {
  LOGF("[error] %s / %s\n", line1, line2 ? line2 : "");
  ledSetState(LED_ERROR);
  displayMessage("ERROR", line1, line2, COLOR_RED);

  // Bring up a CDC-only device so the buffered boot log is still reachable;
  // without this a failed setup leaves no USB at all and nothing to debug.
  logStartUsbSerial();
  USB.begin();

  while (true) {
    logPump();
    ledUpdate(0, 0);
    delay(200);
  }
}

// ---------------------------------------------------------------------------
// first-run provisioning
// ---------------------------------------------------------------------------

static void provisionProgress(const char *what, uint8_t percent) {
  static uint8_t last = 255;
  if (percent == last) return;
  last = percent;

  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", (unsigned)percent);
  displayText(3, 26, pct, COLOR_WHITE, COLOR_BLACK, 2);

  // Progress bar along the bottom.
  int16_t width = (int16_t)((LCD_WIDTH - 6) * percent / 100);
  displayFillRect(3, 58, width, 8, COLOR_GREEN);

  LOGF("[provision] %s %u%%\n", what, (unsigned)percent);
}

static bool provisionFirstRun() {
  LOGF("%s\n", "[provision] no images found, creating defaults");

  if (!SD_MMC.exists(gConfig.imageDir) && !SD_MMC.mkdir(gConfig.imageDir)) {
    LOGF("[provision] cannot create %s\n", gConfig.imageDir.c_str());
    return false;
  }

  String name = "disk1.img";
  String path = gConfig.imageDir + "/" + name;

  displayMessage("FIRST RUN", "0%", "Creating image, please wait",
                 COLOR_YELLOW);
  displayFillRect(3, 58, LCD_WIDTH - 6, 8, COLOR_DARK);

  if (!provisionCreateImage(path, gConfig.defaultImageSizeMiB, "USBSTOR",
                            provisionProgress)) {
    return false;
  }

  if (!provisionWriteDefaultConfig(gConfig.imageDir, name)) {
    LOGF("%s\n", "[provision] could not write default config");
  }
  configLoad(gConfig);

  LOGF("[provision] created %s (%u MiB)\n", path.c_str(),
                (unsigned)gConfig.defaultImageSizeMiB);
  return true;
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

void setup() {
  logInit();
  pinMode(BOOT_PIN, INPUT_PULLUP);

  // Consume the mode flag immediately: maintenance mode is entered only by an
  // explicit request from the previous run, and never survives a power cycle
  // or an unexpected reset.
  gMaintenance = (gBootMode == BOOT_MODE_MAINTENANCE);
  gBootMode = 0;

  ledBegin(8);
  ledSetState(LED_IDLE);
  ledUpdate(0, 0);

  displayBegin(200);
  displayMessage("USB STOR EMU", "Starting", "mounting SD card", COLOR_CYAN);

  if (!storageBegin()) {
    fail("No SD card", "insert card and replug");
  }

  bool haveConfig = configLoad(gConfig);
  if (!haveConfig) {
    LOGF("%s\n", "[config] " CONFIG_PATH " not found, using defaults");
  }

  ledSetEnabled(gConfig.ledEnabled);
  ledBegin(gConfig.ledBrightness);
  ledSetState(LED_IDLE);

  if (!gConfig.displayEnabled) {
    displayEnd();
  } else {
    displaySetBacklight(gConfig.backlight);
  }

  gImageCount = storageListImages(gConfig.imageDir, gImages, STORAGE_MAX_IMAGES);
  LOGF("[images] %u found in %s\n", (unsigned)gImageCount,
                gConfig.imageDir.c_str());
  for (size_t i = 0; i < gImageCount; i++) {
    LOGF("  [%u] %s\n", (unsigned)i, gImages[i].c_str());
  }

  // First run on a blank card: lay down an image and a config so the dongle is
  // usable without ever taking the card out.
  if (!gImageCount) {
    if (!provisionFirstRun()) {
      fail("No images", gConfig.imageDir.c_str());
    }
    gImageCount =
        storageListImages(gConfig.imageDir, gImages, STORAGE_MAX_IMAGES);
    if (!gImageCount) fail("No images", gConfig.imageDir.c_str());
    haveConfig = true;
  }

  if (!haveConfig) {
    // Images exist but no config file: write one naming the first image.
    if (provisionWriteDefaultConfig(gConfig.imageDir, gImages[0])) {
      LOGF("%s\n", "[config] wrote default " CONFIG_PATH);
      configLoad(gConfig);
    }
  }

  LOGF("[config] file=%s dir=%s active='%s'\n", haveConfig ? "yes" : "no",
       gConfig.imageDir.c_str(), gConfig.activeImage.c_str());
  LOGF("[config] vid=%04X pid=%04X vendor='%s' product='%s'\n", gConfig.usb.vid,
       gConfig.usb.pid, gConfig.usb.manufacturer.c_str(),
       gConfig.usb.product.c_str());
  LOGF("[config] readonly=%d capacity_mib=%u scsi='%s'/'%s'/'%s'\n",
       gConfig.usb.readOnly ? 1 : 0, (unsigned)gConfig.usb.capacityMiB,
       gConfig.usb.scsiVendor.c_str(), gConfig.usb.scsiProduct.c_str(),
       gConfig.usb.scsiRevision.c_str());

  if (gMaintenance) {
    // Expose the whole physical card so the host can edit usbstor.ini and add
    // images. This tears down the filesystem mount, so it happens only after
    // the config has been read.
    configApplyMaintenance(gConfig);
    LOGF("[maint] raw card passthrough, pid=%04X product='%s'\n",
         gConfig.usb.pid, gConfig.usb.product.c_str());

    if (!storageUseRawCard()) {
      fail("Raw SD failed", "could not open card");
    }
    gActiveImage = "SD CARD";

    if (!usbMscBegin(gConfig)) {
      fail("USB init failed", "maintenance");
    }
    LOGF("[maint] serving %llu MiB raw\n", storageImageSizeBytes() >> 20);
  } else {
    size_t idx =
        gConfig.activeImage.length() ? indexOfImage(gConfig.activeImage) : 0;
    if (gConfig.activeImage.length() &&
        !gImages[idx].equalsIgnoreCase(gConfig.activeImage)) {
      LOGF("[images] configured active '%s' not found, using '%s'\n",
           gConfig.activeImage.c_str(), gImages[idx].c_str());
    }

    gActiveImage = gImages[idx];

    // Per-image [image:<name>] sections may override any USB or display key.
    configApplyImageOverrides(gConfig, gActiveImage);

    if (!storageOpenImage(gConfig.imageDir, gActiveImage, gConfig.usb.readOnly,
                          gConfig.usb.capacityMiB)) {
      fail("Image open failed", gActiveImage.c_str());
    }

    if (!usbMscBegin(gConfig)) {
      fail("USB init failed", gActiveImage.c_str());
    }
  }

  if (!gConfig.title.length()) gConfig.title = gConfig.usb.product;

  ledSetState(LED_IDLE);
  displayInvalidate();
  displayFill(COLOR_BLACK);
  gRunning = true;

  LOGF("[ready] serving %s (%llu MiB) as %s\n", gActiveImage.c_str(),
                storageImageSizeBytes() >> 20,
                gConfig.usb.readOnly ? "read-only" : "read/write");
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------

void loop() {
  logPump();

  if (!gRunning) {
    delay(100);
    return;
  }

  uint32_t now = millis();
  uint32_t lastRead = storageLastReadMs();
  uint32_t lastWrite = storageLastWriteMs();

  bool reading = lastRead && (now - lastRead) < ACTIVITY_HOLD_MS;
  bool writing = lastWrite && (now - lastWrite) < ACTIVITY_HOLD_MS;
  bool connected = usbMscHostConnected();

  if (gMaintenance) {
    ledSetState(LED_MAINT);
  } else {
    ledSetState(connected ? LED_READY : LED_IDLE);
  }
  ledUpdate(lastRead, lastWrite);

  if (gConfig.displayEnabled) {
    displayStatus(gConfig.title, gActiveImage, storageImageSizeBytes(),
                  gConfig.usb.readOnly, connected, reading, writing);
  }

  // The button is only usable at runtime: GPIO0 held at reset puts the ROM into
  // download mode, so it can never reach this firmware during boot.
  if (!btnDown()) {
    delay(20);
    return;
  }
  delay(BTN_DEBOUNCE_MS);
  if (!btnDown() || !btnWaitRelease()) {
    delay(20);
    return;
  }

  if (gMaintenance) {
    // Any long press leaves maintenance mode and returns to serving an image.
    LOGF("%s\n", "[maint] leaving maintenance mode");
    displayMessage("MAINTENANCE", "Exiting", "restarting...", COLOR_CYAN);
    delay(600);
    restartInto(0);
  }

  LOGF("%s\n", "[select] entering selection");
  storageFlush();
  size_t chosen = runSelector(indexOfImage(gActiveImage));

  if (chosen == SELECT_MAINTENANCE(gImageCount)) {
    displayMessage("MAINTENANCE", "Entering", "restarting...", COLOR_CYAN);
    delay(600);
    restartInto(BOOT_MODE_MAINTENANCE);
  }

  if (!gImages[chosen].equalsIgnoreCase(gActiveImage)) {
    // Descriptors and capacity are fixed at enumeration, so applying another
    // image's settings means persisting the choice and re-enumerating.
    configSaveActiveImage(gImages[chosen]);
    storageCloseImage();
    displayMessage("SWITCHING", gImages[chosen].c_str(), "restarting...",
                   COLOR_CYAN);
    delay(600);
    restartInto(0);
  }

  displayInvalidate();
  displayFill(COLOR_BLACK);
  delay(20);
}
