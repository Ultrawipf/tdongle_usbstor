#include "storage.h"

#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "pins.h"
#include "rawsd.h"

static bool sRawMode = false;
static bool sMounted = false;
static File sImage;
static String sImageName;
static uint64_t sImageSize = 0;
static uint32_t sBlockCount = 0;
static bool sReadOnly = true;

static SemaphoreHandle_t sLock = nullptr;
static volatile uint32_t sLastRead = 0;
static volatile uint32_t sLastWrite = 0;

static inline bool lockTake() {
  return sLock && xSemaphoreTake(sLock, pdMS_TO_TICKS(2000)) == pdTRUE;
}
static inline void lockGive() {
  if (sLock) xSemaphoreGive(sLock);
}

bool storageBegin() {
  if (!sLock) sLock = xSemaphoreCreateMutex();

  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN,
                 SD_D3_PIN);

  // 4-bit mode, never format on failure -- we must not touch a card we cannot
  // read. BOARD_MAX_SDMMC_FREQ is the core's per-board ceiling (40 MHz here).
  if (!SD_MMC.begin("/sdcard", false, false, BOARD_MAX_SDMMC_FREQ, 5)) {
    log_e("SD_MMC 4-bit mount failed, retrying in 1-bit mode");
    SD_MMC.end();
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN,
                   SD_D3_PIN);
    if (!SD_MMC.begin("/sdcard", true, false, BOARD_MAX_SDMMC_FREQ, 5)) {
      log_e("SD_MMC mount failed");
      sMounted = false;
      return false;
    }
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    log_e("no SD card detected");
    SD_MMC.end();
    sMounted = false;
    return false;
  }

  sMounted = true;
  log_i("SD mounted, %llu MiB", SD_MMC.cardSize() >> 20);
  return true;
}

bool storageMounted() { return sMounted; }

uint64_t storageCardSizeBytes() { return sMounted ? SD_MMC.cardSize() : 0; }

static bool hasImageExtension(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".img") || lower.endsWith(".bin") ||
         lower.endsWith(".iso") || lower.endsWith(".dsk");
}

size_t storageListImages(const String &dir, String *out, size_t maxOut) {
  if (!sMounted || !maxOut) return 0;

  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) {
    log_w("image dir %s not found", dir.c_str());
    if (d) d.close();
    return 0;
  }

  size_t n = 0;
  while (n < maxOut) {
    File e = d.openNextFile();
    if (!e) break;
    if (!e.isDirectory()) {
      // File::name() is the bare name on core 2.x when opened from a dir.
      String name = e.name();
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (name.length() && !name.startsWith(".") && hasImageExtension(name) &&
          e.size() >= STORAGE_BLOCK_SIZE) {
        out[n++] = name;
      }
    }
    e.close();
  }
  d.close();

  // Insertion sort -- n is tiny and this keeps the button order stable.
  for (size_t i = 1; i < n; i++) {
    String key = out[i];
    String keyLower = key;
    keyLower.toLowerCase();
    size_t j = i;
    while (j > 0) {
      String cmp = out[j - 1];
      cmp.toLowerCase();
      if (cmp.compareTo(keyLower) <= 0) break;
      out[j] = out[j - 1];
      j--;
    }
    out[j] = key;
  }

  return n;
}

bool storageOpenImage(const String &dir, const String &name, bool readOnly,
                      uint32_t capacityMiB) {
  storageCloseImage();
  if (!sMounted || !name.length()) return false;

  String path = dir;
  if (!path.endsWith("/")) path += "/";
  path += name;

  sImage = SD_MMC.open(path, readOnly ? "r" : "r+");
  if (!sImage && !readOnly) {
    // Card or file is physically read-only: fall back rather than fail outright.
    log_w("cannot open %s writable, falling back to read-only", path.c_str());
    sImage = SD_MMC.open(path, "r");
    readOnly = true;
  }
  if (!sImage) {
    log_e("cannot open image %s", path.c_str());
    return false;
  }
  if (sImage.isDirectory()) {
    sImage.close();
    return false;
  }

  sReadOnly = readOnly;
  sImageName = name;
  sImageSize = sImage.size();
  sBlockCount = (uint32_t)(sImageSize / STORAGE_BLOCK_SIZE);

  if (capacityMiB) {
    uint64_t wanted = (uint64_t)capacityMiB << 20;
    uint32_t wantedBlocks = (uint32_t)(wanted / STORAGE_BLOCK_SIZE);
    if (wantedBlocks && wantedBlocks < sBlockCount) {
      // Only ever shrink: reporting blocks that are not backed by the file
      // would hand the host storage that silently fails on access.
      log_i("capacity clamped to %u MiB by config", (unsigned)capacityMiB);
      sBlockCount = wantedBlocks;
    } else if (wantedBlocks > sBlockCount) {
      log_w("capacity_mib=%u exceeds image size, ignored", (unsigned)capacityMiB);
    }
  }

  if (!sBlockCount) {
    log_e("image %s is smaller than one block", path.c_str());
    sImage.close();
    return false;
  }

  log_i("image %s: %u blocks (%llu MiB), %s", name.c_str(),
        (unsigned)sBlockCount, (sImageSize >> 20), sReadOnly ? "ro" : "rw");
  return true;
}

void storageCloseImage() {
  if (sImage) {
    sImage.flush();
    sImage.close();
  }
  sImageName = "";
  sImageSize = 0;
  sBlockCount = 0;
}

bool storageUseRawCard() {
  if (sRawMode) return true;
  storageCloseImage();
  if (!rawSdBegin()) return false;
  sRawMode = true;
  sMounted = false;  // the VFS mount is gone; only raw access remains
  sReadOnly = false;
  sImageName = "SD CARD";
  return true;
}

bool storageIsRawCard() { return sRawMode; }

uint32_t storageBlockCount() {
  return sRawMode ? rawSdBlockCount() : sBlockCount;
}
uint64_t storageImageSizeBytes() {
  return sRawMode ? rawSdSizeBytes() : sImageSize;
}
const String &storageImageName() { return sImageName; }
uint32_t storageLastReadMs() { return sLastRead; }
uint32_t storageLastWriteMs() { return sLastWrite; }

// Range check shared by read and write. Returns the clamped size, or 0 if the
// request lies entirely outside the reported capacity.
static uint32_t clampToImage(uint32_t lba, uint32_t offset, uint32_t size,
                             uint64_t *absOut) {
  if (lba >= sBlockCount) return 0;
  uint64_t abs = (uint64_t)lba * STORAGE_BLOCK_SIZE + offset;
  uint64_t limit = (uint64_t)sBlockCount * STORAGE_BLOCK_SIZE;
  if (abs >= limit) return 0;
  if (abs + size > limit) size = (uint32_t)(limit - abs);
  *absOut = abs;
  return size;
}

int32_t storageRead(uint32_t lba, uint32_t offset, void *buffer,
                    uint32_t size) {
  if (sRawMode) {
    if (!lockTake()) return -1;
    int32_t r = rawSdRead(lba, offset, buffer, size);
    lockGive();
    if (r > 0) sLastRead = millis();
    return r;
  }

  if (!sImage || !buffer) return -1;

  uint64_t abs = 0;
  size = clampToImage(lba, offset, size, &abs);
  if (!size) return -1;

  if (!lockTake()) return -1;
  int32_t result = -1;
  if (sImage.seek(abs)) {
    int r = sImage.read((uint8_t *)buffer, size);
    if (r >= 0) result = r;
  }
  lockGive();

  if (result > 0) sLastRead = millis();
  return result;
}

int32_t storageWrite(uint32_t lba, uint32_t offset, const uint8_t *buffer,
                     uint32_t size) {
  if (sRawMode) {
    if (!lockTake()) return -1;
    int32_t w = rawSdWrite(lba, offset, buffer, size);
    lockGive();
    if (w > 0) sLastWrite = millis();
    return w;
  }

  if (!sImage || !buffer) return -1;
  if (sReadOnly) return -1;  // belt and braces alongside tud_msc_is_writable_cb

  uint64_t abs = 0;
  size = clampToImage(lba, offset, size, &abs);
  if (!size) return -1;

  if (!lockTake()) return -1;
  int32_t result = -1;
  if (sImage.seek(abs)) {
    size_t w = sImage.write(buffer, size);
    if (w) result = (int32_t)w;
  }
  lockGive();

  if (result > 0) sLastWrite = millis();
  return result;
}

void storageFlush() {
  if (!sImage || sReadOnly) return;
  if (!lockTake()) return;
  sImage.flush();
  lockGive();
}
