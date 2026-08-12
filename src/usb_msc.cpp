#include "usb_msc.h"

#include <USB.h>
#include <USBMSC.h>
#include <esp_mac.h>

#include "logbuf.h"
#include "storage.h"

static USBMSC sMsc;
static bool sReadOnly = true;
static volatile bool sConnected = false;
static String sSerial;

// ---------------------------------------------------------------------------
// SCSI write-protect
//
// The Arduino core does not implement tud_msc_is_writable_cb, so TinyUSB's weak
// default (always writable) applies. Defining it here overrides that weak
// symbol at link time and makes MODE SENSE report the write-protect bit, which
// is what actually makes a host mount the volume read-only.
// ---------------------------------------------------------------------------
extern "C" bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  return !sReadOnly;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer,
                      uint32_t bufsize) {
  return storageRead(lba, offset, buffer, bufsize);
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer,
                       uint32_t bufsize) {
  if (sReadOnly) return -1;
  return storageWrite(lba, offset, buffer, bufsize);
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition;
  // The host ejecting the volume is our cue to flush any buffered writes.
  if (load_eject && !start) storageFlush();
  return true;
}

static void onUsbEvent(void *arg, esp_event_base_t base, int32_t id,
                       void *data) {
  (void)arg;
  (void)data;
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT:
    case ARDUINO_USB_RESUME_EVENT:
      sConnected = true;
      break;
    case ARDUINO_USB_STOPPED_EVENT:
    case ARDUINO_USB_SUSPEND_EVENT:
      sConnected = false;
      storageFlush();
      break;
    default:
      break;
  }
}

// Truncates to the SCSI INQUIRY field widths (8/16/4) before handing over.
static void copyFixed(const String &src, char *dst, size_t width) {
  size_t n = src.length() < width ? src.length() : width;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

static String defaultSerial() {
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  char buf[17];
  snprintf(buf, sizeof(buf), "TDS3%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool usbMscBegin(const AppConfig &cfg) {
  uint32_t blocks = storageBlockCount();
  if (!blocks) {
    log_e("usbMscBegin: no image open");
    return false;
  }

  sReadOnly = cfg.usb.readOnly;
  sSerial = cfg.usb.serial.length() ? cfg.usb.serial : defaultSerial();

  // Descriptor fields must all be set before USB.begin().
  USB.VID(cfg.usb.vid);
  USB.PID(cfg.usb.pid);
  USB.manufacturerName(cfg.usb.manufacturer.c_str());
  USB.productName(cfg.usb.product.c_str());
  USB.serialNumber(sSerial.c_str());
  USB.onEvent(onUsbEvent);

  char vendor[9], product[17], revision[5];
  copyFixed(cfg.usb.scsiVendor, vendor, 8);
  copyFixed(cfg.usb.scsiProduct, product, 16);
  copyFixed(cfg.usb.scsiRevision, revision, 4);

  sMsc.vendorID(vendor);
  sMsc.productID(product);
  sMsc.productRevision(revision);
  sMsc.onRead(onRead);
  sMsc.onWrite(onWrite);
  sMsc.onStartStop(onStartStop);
  sMsc.mediaPresent(true);

  if (!sMsc.begin(blocks, STORAGE_BLOCK_SIZE)) {
    log_e("USBMSC begin failed");
    return false;
  }

  // The CDC console joins the same composite device; it must be registered
  // before USB.begin(), which is the point of no return for the descriptors.
  logStartUsbSerial();

  USB.begin();

  log_i("USB %04X:%04X '%s' / '%s' serial '%s' %s, %u blocks", cfg.usb.vid,
        cfg.usb.pid, cfg.usb.manufacturer.c_str(), cfg.usb.product.c_str(),
        sSerial.c_str(), sReadOnly ? "read-only" : "read/write",
        (unsigned)blocks);
  return true;
}

bool usbMscHostConnected() { return sConnected; }

const String &usbMscSerial() { return sSerial; }
