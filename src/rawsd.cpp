#include "rawsd.h"

#include <SD_MMC.h>
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <sdmmc_cmd.h>

#include "pins.h"
#include "storage.h"

// TinyUSB hands us at most CONFIG_TINYUSB_MSC_BUFSIZE (4 KiB) per callback.
// A request straddling sector boundaries touches at most 9 sectors; 16 gives
// headroom and keeps the DMA buffer a round 8 KiB.
#define BOUNCE_SECTORS 16

static sdmmc_card_t sCard;
static bool sReady = false;
static uint8_t *sBounce = nullptr;

bool rawSdBegin() {
  if (sReady) return true;

  // Drop the filesystem mount; the SDMMC host is re-initialised from scratch.
  SD_MMC.end();

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_4BIT;
  host.slot = SDMMC_HOST_SLOT_1;
  host.max_freq_khz = BOARD_MAX_SDMMC_FREQ;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = (gpio_num_t)SD_CLK_PIN;
  slot.cmd = (gpio_num_t)SD_CMD_PIN;
  slot.d0 = (gpio_num_t)SD_D0_PIN;
  slot.d1 = (gpio_num_t)SD_D1_PIN;
  slot.d2 = (gpio_num_t)SD_D2_PIN;
  slot.d3 = (gpio_num_t)SD_D3_PIN;
  slot.width = 4;

  esp_err_t err = sdmmc_host_init();
  if (err != ESP_OK) {
    log_e("sdmmc_host_init failed: %d", err);
    return false;
  }

  err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
  if (err != ESP_OK) {
    log_e("sdmmc_host_init_slot failed: %d", err);
    sdmmc_host_deinit();
    return false;
  }

  err = sdmmc_card_init(&host, &sCard);
  if (err != ESP_OK) {
    // Retry at 1-bit, matching the fallback in storageBegin().
    log_w("4-bit card init failed (%d), retrying 1-bit", err);
    host.flags = SDMMC_HOST_FLAG_1BIT;
    slot.width = 1;
    sdmmc_host_deinit();
    if (sdmmc_host_init() != ESP_OK ||
        sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot) != ESP_OK ||
        sdmmc_card_init(&host, &sCard) != ESP_OK) {
      log_e("raw card init failed");
      sdmmc_host_deinit();
      return false;
    }
  }

  if (sCard.csd.sector_size != STORAGE_BLOCK_SIZE) {
    // The MSC layer reports 512-byte blocks; a card with a different native
    // sector size would need translation we do not implement.
    log_e("unsupported sector size %d", sCard.csd.sector_size);
    sdmmc_host_deinit();
    return false;
  }

  sBounce = (uint8_t *)heap_caps_malloc(BOUNCE_SECTORS * STORAGE_BLOCK_SIZE,
                                        MALLOC_CAP_DMA);
  if (!sBounce) {
    log_e("no DMA memory for raw SD buffer");
    sdmmc_host_deinit();
    return false;
  }

  sReady = true;
  log_i("raw SD ready: %d sectors (%llu MiB)", sCard.csd.capacity,
        ((uint64_t)sCard.csd.capacity * STORAGE_BLOCK_SIZE) >> 20);
  return true;
}

bool rawSdReady() { return sReady; }

uint32_t rawSdBlockCount() {
  return sReady ? (uint32_t)sCard.csd.capacity : 0;
}

uint64_t rawSdSizeBytes() {
  return sReady ? (uint64_t)sCard.csd.capacity * STORAGE_BLOCK_SIZE : 0;
}

// Clamps a byte range to the card and returns the sector span covering it.
static bool spanFor(uint32_t lba, uint32_t offset, uint32_t *size,
                    uint64_t *abs, uint32_t *first, uint32_t *count) {
  if (!sReady) return false;
  uint64_t total = rawSdSizeBytes();
  uint64_t a = (uint64_t)lba * STORAGE_BLOCK_SIZE + offset;
  if (a >= total) return false;
  if (a + *size > total) *size = (uint32_t)(total - a);
  if (!*size) return false;

  uint32_t f = (uint32_t)(a / STORAGE_BLOCK_SIZE);
  uint32_t l = (uint32_t)((a + *size - 1) / STORAGE_BLOCK_SIZE);
  uint32_t n = l - f + 1;
  if (n > BOUNCE_SECTORS) return false;

  *abs = a;
  *first = f;
  *count = n;
  return true;
}

int32_t rawSdRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
  uint64_t abs;
  uint32_t first, count;
  if (!buffer || !spanFor(lba, offset, &size, &abs, &first, &count)) return -1;

  if (sdmmc_read_sectors(&sCard, sBounce, first, count) != ESP_OK) return -1;
  memcpy(buffer, sBounce + (abs - (uint64_t)first * STORAGE_BLOCK_SIZE), size);
  return (int32_t)size;
}

int32_t rawSdWrite(uint32_t lba, uint32_t offset, const uint8_t *buffer,
                   uint32_t size) {
  uint64_t abs;
  uint32_t first, count;
  if (!buffer || !spanFor(lba, offset, &size, &abs, &first, &count)) return -1;

  uint32_t head = (uint32_t)(abs - (uint64_t)first * STORAGE_BLOCK_SIZE);
  bool aligned = (head == 0) && (size % STORAGE_BLOCK_SIZE == 0);
  if (!aligned) {
    // Preserve the bytes of the first/last sector that are not being written.
    if (sdmmc_read_sectors(&sCard, sBounce, first, count) != ESP_OK) return -1;
  }

  memcpy(sBounce + head, buffer, size);
  if (sdmmc_write_sectors(&sCard, sBounce, first, count) != ESP_OK) return -1;
  return (int32_t)size;
}
