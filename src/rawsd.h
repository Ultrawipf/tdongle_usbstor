// Raw SDMMC block access for maintenance mode.
//
// Normal operation serves an image file through the FATFS/VFS layer. In
// maintenance mode the whole physical card is exposed instead, partition table
// included, so the host can edit /usbstor.ini and drop in new images. That
// needs block access below the filesystem, so the VFS mount is torn down and
// the SDMMC host is driven directly.
#pragma once

#include <Arduino.h>

// Unmounts SD_MMC and re-initialises the SDMMC host for raw access.
// Nothing else may touch SD_MMC afterwards until the next reboot.
bool rawSdBegin();

bool rawSdReady();
uint32_t rawSdBlockCount();
uint64_t rawSdSizeBytes();

// Byte-granular access built on whole-sector transfers. Unaligned writes are
// read-modify-write. Callers must serialise; storage.cpp holds the lock.
int32_t rawSdRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size);
int32_t rawSdWrite(uint32_t lba, uint32_t offset, const uint8_t *buffer,
                   uint32_t size);
