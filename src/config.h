// Configuration model + INI reader/writer for /usbstor.ini on the SD card.
#pragma once

#include <Arduino.h>

#define CONFIG_PATH "/usbstor.ini"

struct UsbConfig {
  uint16_t vid = 0x303A;
  uint16_t pid = 0x4002;
  String manufacturer = "LilyGO";
  String product = "T-Dongle-S3 Disk";
  String serial = "";  // empty -> derived from the eFuse MAC
  bool readOnly = false;

  // SCSI INQUIRY strings. The spec caps these at 8/16/4 characters; longer
  // values are truncated when handed to the MSC class.
  String scsiVendor = "LILYGO";
  String scsiProduct = "FlexDisk";
  String scsiRevision = "1.0";

  // Reported capacity override in MiB. 0 means "use the real image size".
  // Only shrinking is honoured -- see storage.cpp.
  uint32_t capacityMiB = 0;
};

struct AppConfig {
  UsbConfig usb;

  String imageDir = "/images";
  String activeImage = "";  // file name inside imageDir, empty -> first found

  // Size of the image created on first run when the directory is empty.
  uint32_t defaultImageSizeMiB = 64;

  bool displayEnabled = true;
  uint8_t backlight = 200;  // 0..255
  String title = "";        // empty -> falls back to usb.product

  bool ledEnabled = true;
  uint8_t ledBrightness = 8;  // APA102 global current, 0..31
};

// Parses CONFIG_PATH from the SD card into `cfg`. Missing file or missing keys
// leave the defaults in place; returns false only if the file could not be read
// at all (the caller still gets a usable default config).
bool configLoad(AppConfig &cfg);

// Re-reads CONFIG_PATH and applies any [image:<name>] section matching
// `imageName` on top of `cfg`. Call after the active image has been resolved.
void configApplyImageOverrides(AppConfig &cfg, const String &imageName);

// Switches `cfg` to the maintenance identity (distinct product/PID, always
// writable, no capacity clamp), then applies any [maintenance] section.
void configApplyMaintenance(AppConfig &cfg);

// Rewrites the `active =` key in the [content] section so a selection made with
// the button survives a power cycle. Returns false if the file could not be
// rewritten (e.g. read-only card); a failure is non-fatal.
bool configSaveActiveImage(const String &imageName);

// Helpers shared with the INI parser.
String iniTrim(const String &s);
bool iniParseBool(const String &v, bool fallback);
uint32_t iniParseNumber(const String &v, uint32_t fallback);
