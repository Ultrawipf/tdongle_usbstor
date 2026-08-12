// SD card bring-up and image-file backed block device.
#pragma once

#include <Arduino.h>

#define STORAGE_BLOCK_SIZE 512
#define STORAGE_MAX_IMAGES 16

// Mounts the SD card over SDMMC in 4-bit mode. Returns false if no card is
// present or the card could not be mounted.
bool storageBegin();

bool storageMounted();
uint64_t storageCardSizeBytes();

// Scans <dir> for *.img / *.bin files. Names are returned sorted so the button
// cycles through them in a stable order. Returns the number found.
size_t storageListImages(const String &dir, String *out, size_t maxOut);

// Opens <dir>/<name> as the active block device. `readOnly` decides whether the
// file is opened for writing at all -- a read-only config can therefore never
// modify the card even if a host ignores the write-protect bit.
// `capacityMiB` of 0 uses the real file size; a smaller non-zero value shrinks
// the reported capacity, a larger one is ignored (we never report space that
// does not exist).
bool storageOpenImage(const String &dir, const String &name, bool readOnly,
                      uint32_t capacityMiB);

void storageCloseImage();

// Switches the block backend to the whole physical SD card (maintenance mode).
// Tears down the filesystem mount, so no further SD_MMC use is possible until
// reboot. All storage* accessors below then refer to the raw card.
bool storageUseRawCard();
bool storageIsRawCard();

uint32_t storageBlockCount();
uint64_t storageImageSizeBytes();
const String &storageImageName();

// Block access used by the MSC callbacks. Both return the number of bytes
// transferred or -1 on error. They are internally serialised, so they are safe
// to call from the TinyUSB task while loop() touches the card.
int32_t storageRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size);
int32_t storageWrite(uint32_t lba, uint32_t offset, const uint8_t *buffer,
                     uint32_t size);
void storageFlush();

// Activity counters, consumed by the LED/display to blink on access.
uint32_t storageLastReadMs();
uint32_t storageLastWriteMs();
