#include "config.h"

#include <SD_MMC.h>

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

String iniTrim(const String &s) {
  int b = 0;
  int e = s.length();
  while (b < e && isspace((unsigned char)s[b])) b++;
  while (e > b && isspace((unsigned char)s[e - 1])) e--;
  return s.substring(b, e);
}

bool iniParseBool(const String &v, bool fallback) {
  String t = v;
  t.toLowerCase();
  if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
  if (t == "0" || t == "false" || t == "no" || t == "off") return false;
  return fallback;
}

uint32_t iniParseNumber(const String &v, uint32_t fallback) {
  String t = iniTrim(v);
  if (!t.length()) return fallback;
  char *end = nullptr;
  // strtoul with base 0 handles 0x-prefixed hex as well as plain decimal.
  unsigned long n = strtoul(t.c_str(), &end, 0);
  if (end == t.c_str()) return fallback;
  return (uint32_t)n;
}

// Strips a trailing comment that is not inside quotes, then unquotes the value.
static String cleanValue(const String &raw) {
  String v = raw;
  bool quoted = v.length() >= 2 && ((v[0] == '"' && v[v.length() - 1] == '"') ||
                                    (v[0] == '\'' && v[v.length() - 1] == '\''));
  if (!quoted) {
    for (int i = 0; i < (int)v.length(); i++) {
      if (v[i] == ';' || v[i] == '#') {
        v = v.substring(0, i);
        break;
      }
    }
    v = iniTrim(v);
  } else {
    v = v.substring(1, v.length() - 1);
  }
  return v;
}

// ---------------------------------------------------------------------------
// generic line-oriented parser
// ---------------------------------------------------------------------------

typedef void (*IniHandler)(const String &section, const String &key,
                           const String &value, void *ctx);

static bool iniParseFile(const char *path, IniHandler handler, void *ctx) {
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;

  String section = "";
  String line;
  line.reserve(128);

  while (f.available()) {
    line = f.readStringUntil('\n');
    line = iniTrim(line);
    if (!line.length()) continue;
    if (line[0] == ';' || line[0] == '#') continue;

    if (line[0] == '[') {
      int close = line.indexOf(']');
      if (close > 0) {
        section = iniTrim(line.substring(1, close));
        section.toLowerCase();
      }
      continue;
    }

    int eq = line.indexOf('=');
    if (eq < 0) continue;

    String key = iniTrim(line.substring(0, eq));
    key.toLowerCase();
    String value = cleanValue(iniTrim(line.substring(eq + 1)));
    if (!key.length()) continue;

    handler(section, key, value, ctx);
  }

  f.close();
  return true;
}

// ---------------------------------------------------------------------------
// mapping INI keys onto AppConfig
// ---------------------------------------------------------------------------

// Applies one key/value pair. `usbSection` names the section that carries USB
// settings, which is "usb" for the base config and "image:<name>" for the
// per-image override pass.
// In an override pass the section has already been matched by the caller, so
// USB and display keys are accepted regardless of the section's name. This is
// what lets [image:<name>] and [maintenance] share one implementation.
static void applyPair(AppConfig &cfg, const String &section, const String &key,
                      const String &value, bool basePass) {
  if (section == "usb" || !basePass) {
    UsbConfig &u = cfg.usb;
    if (key == "vid") u.vid = (uint16_t)iniParseNumber(value, u.vid);
    else if (key == "pid") u.pid = (uint16_t)iniParseNumber(value, u.pid);
    else if (key == "vendor" || key == "manufacturer") u.manufacturer = value;
    else if (key == "product") u.product = value;
    else if (key == "serial") u.serial = value;
    else if (key == "readonly") u.readOnly = iniParseBool(value, u.readOnly);
    else if (key == "scsi_vendor") u.scsiVendor = value;
    else if (key == "scsi_product") u.scsiProduct = value;
    else if (key == "scsi_revision") u.scsiRevision = value;
    else if (key == "capacity_mib") u.capacityMiB = iniParseNumber(value, u.capacityMiB);
  }

  if (section == "content" && basePass) {
    if (key == "dir") cfg.imageDir = value;
    else if (key == "active") cfg.activeImage = value;
    else if (key == "default_size_mib") {
      cfg.defaultImageSizeMiB = iniParseNumber(value, cfg.defaultImageSizeMiB);
    }
  }

  if (section == "display" || !basePass) {
    if (key == "enabled") cfg.displayEnabled = iniParseBool(value, cfg.displayEnabled);
    else if (key == "backlight") cfg.backlight = (uint8_t)iniParseNumber(value, cfg.backlight);
    else if (key == "title") cfg.title = value;
  }

  if (section == "led") {
    if (key == "enabled") cfg.ledEnabled = iniParseBool(value, cfg.ledEnabled);
    else if (key == "brightness") {
      uint32_t b = iniParseNumber(value, cfg.ledBrightness);
      cfg.ledBrightness = (uint8_t)(b > 31 ? 31 : b);
    }
  }
}

struct BaseCtx {
  AppConfig *cfg;
};

static void baseHandler(const String &section, const String &key,
                        const String &value, void *ctx) {
  BaseCtx *c = (BaseCtx *)ctx;
  applyPair(*c->cfg, section, key, value, /*basePass=*/true);
}

struct OverrideCtx {
  AppConfig *cfg;
  String wanted;  // "image:<lowercased name>"
};

static void overrideHandler(const String &section, const String &key,
                            const String &value, void *ctx) {
  OverrideCtx *c = (OverrideCtx *)ctx;
  if (section != c->wanted) return;
  applyPair(*c->cfg, section, key, value, /*basePass=*/false);
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

bool configLoad(AppConfig &cfg) {
  BaseCtx ctx{&cfg};
  bool ok = iniParseFile(CONFIG_PATH, baseHandler, &ctx);

  // Normalise the image directory to a leading-slash, no-trailing-slash form.
  if (!cfg.imageDir.startsWith("/")) cfg.imageDir = "/" + cfg.imageDir;
  while (cfg.imageDir.length() > 1 && cfg.imageDir.endsWith("/")) {
    cfg.imageDir.remove(cfg.imageDir.length() - 1);
  }
  return ok;
}

void configApplyImageOverrides(AppConfig &cfg, const String &imageName) {
  if (!imageName.length()) return;
  OverrideCtx ctx;
  ctx.cfg = &cfg;
  ctx.wanted = "image:" + imageName;
  ctx.wanted.toLowerCase();
  iniParseFile(CONFIG_PATH, overrideHandler, &ctx);
}

void configApplyMaintenance(AppConfig &cfg) {
  // Defaults first: a distinct product name and PID so the host does not reuse
  // cached geometry from the normal device, and writes always enabled --
  // maintenance mode exists precisely to edit the card.
  cfg.usb.product = cfg.usb.product + " SD";
  cfg.usb.pid = (uint16_t)(cfg.usb.pid + 1);
  cfg.usb.scsiProduct = "SD CARD";
  cfg.usb.readOnly = false;
  cfg.usb.capacityMiB = 0;  // never clamp the real card
  cfg.title = "MAINTENANCE";

  // Then let an explicit [maintenance] section override any of it.
  OverrideCtx ctx;
  ctx.cfg = &cfg;
  ctx.wanted = "maintenance";
  iniParseFile(CONFIG_PATH, overrideHandler, &ctx);

  cfg.usb.capacityMiB = 0;
}

bool configSaveActiveImage(const String &imageName) {
  // Read the whole file, swap the active= line inside [content], write it back.
  // Config files are a few hundred bytes, so buffering in RAM is fine.
  String out;
  bool replaced = false;
  bool sawContent = false;

  File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
  if (f) {
    out.reserve(f.size() + 64);
    String section = "";
    while (f.available()) {
      String raw = f.readStringUntil('\n');
      // readStringUntil drops the '\n' but keeps a '\r' from CRLF files.
      while (raw.length() && (raw[raw.length() - 1] == '\r')) {
        raw.remove(raw.length() - 1);
      }
      String t = iniTrim(raw);

      if (t.length() && t[0] == '[') {
        // Leaving [content] without having seen an active= key: add one.
        if (section == "content" && !replaced) {
          out += "active = " + imageName + "\n";
          replaced = true;
        }
        int close = t.indexOf(']');
        if (close > 0) {
          section = iniTrim(t.substring(1, close));
          section.toLowerCase();
          if (section == "content") sawContent = true;
        }
        out += raw + "\n";
        continue;
      }

      if (section == "content" && !replaced) {
        int eq = t.indexOf('=');
        if (eq > 0) {
          String key = iniTrim(t.substring(0, eq));
          key.toLowerCase();
          if (key == "active") {
            out += "active = " + imageName + "\n";
            replaced = true;
            continue;
          }
        }
      }
      out += raw + "\n";
    }
    if (section == "content" && !replaced) {
      out += "active = " + imageName + "\n";
      replaced = true;
    }
    f.close();
  }

  if (!replaced) {
    if (!sawContent) out += "\n[content]\n";
    out += "active = " + imageName + "\n";
  }

  File w = SD_MMC.open(CONFIG_PATH, FILE_WRITE);
  if (!w) return false;
  size_t written = w.print(out);
  w.close();
  return written == out.length();
}
