// First-run provisioning: writes a default config and creates a formatted
// image so a blank SD card yields a working stick without a card reader.
#pragma once

#include <Arduino.h>

// Progress callback, 0..100.
typedef void (*ProvisionProgress)(const char *what, uint8_t percent);

// Writes CONFIG_PATH with commented defaults. Returns false on write failure.
bool provisionWriteDefaultConfig(const String &imageDir, const String &imageName);

// Creates <path> of <sizeMiB> MiB containing an MBR and one FAT16 partition,
// ready to mount. Sizes are clamped to 8..1024 MiB (the sane FAT16 range).
bool provisionCreateImage(const String &path, uint32_t sizeMiB,
                          const char *volumeLabel, ProvisionProgress progress);
