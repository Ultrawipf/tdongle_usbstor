#include "provision.h"

#include <SD_MMC.h>

#include "config.h"
#include "storage.h"

#define PART_START_LBA 2048  // 1 MiB aligned, the usual layout for a USB stick
#define ROOT_ENTRIES 512
#define NUM_FATS 2
#define RESERVED_SECTORS 1
#define CHUNK_SECTORS 16  // 8 KiB write chunks

static inline void put16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

static inline void put32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

// ---------------------------------------------------------------------------
// default config
// ---------------------------------------------------------------------------

bool provisionWriteDefaultConfig(const String &imageDir,
                                 const String &imageName) {
  File f = SD_MMC.open(CONFIG_PATH, FILE_WRITE);
  if (!f) return false;

  f.print(
      "; usb_stor_emu configuration\n"
      "; To edit this file over USB, long-press BOOT while the dongle is\n"
      "; running and select MAINTENANCE: the whole SD card is then exposed.\n"
      "; Numbers accept decimal or 0x-prefixed hex. Lines starting with ; or #\n"
      "; are comments.\n"
      "\n"
      "[usb]\n"
      "; USB device descriptor - what the host sees in Device Manager / lsusb\n"
      "vid      = 0x303A\n"
      "pid      = 0x4002\n"
      "vendor   = LilyGO\n"
      "product  = T-Dongle-S3 Disk\n"
      "; serial: leave empty to derive a stable one from the chip MAC\n"
      "serial   =\n"
      "; readonly = true presents the volume write-protected to the host\n"
      "readonly = false\n"
      "\n"
      "; SCSI INQUIRY strings (max 8 / 16 / 4 characters)\n"
      "scsi_vendor   = LILYGO\n"
      "scsi_product  = FlexDisk\n"
      "scsi_revision = 1.0\n"
      "\n"
      "; Report a smaller capacity than the image actually has. 0 = full size.\n"
      "capacity_mib = 0\n"
      "\n"
      "[content]\n"
      "; Directory scanned for *.img / *.bin / *.iso / *.dsk files\n");
  f.printf("dir    = %s\n", imageDir.c_str());
  f.print("; Image served over USB. Long-press BOOT while running to pick\n"
          "; another one; the choice is written back here.\n");
  f.printf("active = %s\n", imageName.c_str());

  f.print(
      "\n"
      "[display]\n"
      "enabled   = true\n"
      "backlight = 200\n"
      "; title: shown in the header. Empty falls back to the product name.\n"
      "title     =\n"
      "\n"
      "[led]\n"
      "enabled    = true\n"
      "; APA102 global current, 0..31\n"
      "brightness = 8\n"
      "\n"
      "; Per-image overrides. The section name must match the image file name.\n"
      "; Any key from [usb] or [display] is accepted; anything not listed falls\n"
      "; back to [usb], and an image with no section uses [usb] unchanged.\n"
      "; vendor/product/serial accept up to 125 characters (USB protocol limit).\n"
      ";\n"
      "; [image:secret.img]\n"
      "; product  = Secure Volume\n"
      "; vid      = 0x1234\n"
      "; pid      = 0x5678\n"
      "; readonly = true\n"
      "; title    = SECURE\n"
      "\n"
      "; Maintenance mode exposes the whole SD card. By default it uses pid+1\n"
      "; and appends ' SD' to the product name; override that here.\n"
      ";\n"
      "; [maintenance]\n"
      "; product = Dongle Service Mode\n"
      "; pid     = 0x4010\n");

  bool ok = f.size() > 0;
  f.close();
  return ok;
}

// ---------------------------------------------------------------------------
// FAT16 image creation
// ---------------------------------------------------------------------------

struct Fat16Geometry {
  uint32_t totalSectors;
  uint32_t partStart;
  uint32_t partSectors;
  uint8_t sectorsPerCluster;
  uint32_t fatSectors;
  uint32_t rootSectors;
  uint32_t clusters;
};

// Picks a cluster size and FAT length that satisfy the FAT16 cluster-count
// bounds (4085 .. 65524) for the given partition size.
static bool computeGeometry(uint32_t totalSectors, Fat16Geometry *g) {
  g->totalSectors = totalSectors;
  g->partStart = PART_START_LBA;
  if (totalSectors <= PART_START_LBA) return false;
  g->partSectors = totalSectors - PART_START_LBA;
  g->rootSectors = (ROOT_ENTRIES * 32 + 511) / 512;

  // 2 KiB clusters unless the volume is too large to address with 16 bits.
  uint32_t spc = 4;
  while (spc < 128 && (g->partSectors / spc) > 65524) spc <<= 1;
  if ((g->partSectors / spc) > 65524) return false;
  g->sectorsPerCluster = (uint8_t)spc;

  // The FAT length depends on the cluster count, which depends on the FAT
  // length. Two or three iterations converge.
  uint32_t fatSectors = 1;
  for (int i = 0; i < 8; i++) {
    uint32_t overhead = RESERVED_SECTORS + g->rootSectors + NUM_FATS * fatSectors;
    if (overhead >= g->partSectors) return false;
    uint32_t dataSectors = g->partSectors - overhead;
    uint32_t clusters = dataSectors / spc;
    uint32_t need = ((clusters + 2) * 2 + 511) / 512;
    if (need == fatSectors) break;
    fatSectors = need;
  }

  uint32_t overhead = RESERVED_SECTORS + g->rootSectors + NUM_FATS * fatSectors;
  if (overhead >= g->partSectors) return false;
  g->fatSectors = fatSectors;
  g->clusters = (g->partSectors - overhead) / spc;

  // Below 4085 clusters a volume is FAT12, which we do not emit.
  if (g->clusters < 4085 || g->clusters > 65524) return false;
  return true;
}

static void buildMbr(uint8_t *sec, const Fat16Geometry &g) {
  memset(sec, 0, 512);
  uint8_t *e = sec + 446;  // first partition entry
  e[0] = 0x00;             // not bootable
  // CHS fields are meaningless for LBA-addressed media; the canonical
  // "ignore me, use LBA" filler is 0xFE/0xFF/0xFF.
  e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
  e[4] = 0x0E;             // FAT16 LBA
  e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
  put32(e + 8, g.partStart);
  put32(e + 12, g.partSectors);
  sec[510] = 0x55;
  sec[511] = 0xAA;
}

static void buildBootSector(uint8_t *sec, const Fat16Geometry &g,
                            const char *label, uint32_t volumeId) {
  memset(sec, 0, 512);
  sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;      // jump
  memcpy(sec + 3, "MSDOS5.0", 8);                    // OEM name
  put16(sec + 11, 512);                              // bytes per sector
  sec[13] = g.sectorsPerCluster;
  put16(sec + 14, RESERVED_SECTORS);
  sec[16] = NUM_FATS;
  put16(sec + 17, ROOT_ENTRIES);
  // total_sectors_16 is used when it fits, otherwise total_sectors_32.
  if (g.partSectors < 0x10000) {
    put16(sec + 19, (uint16_t)g.partSectors);
    put32(sec + 32, 0);
  } else {
    put16(sec + 19, 0);
    put32(sec + 32, g.partSectors);
  }
  sec[21] = 0xF8;                                    // fixed disk media
  put16(sec + 22, (uint16_t)g.fatSectors);
  put16(sec + 24, 63);                               // sectors per track
  put16(sec + 26, 255);                              // heads
  put32(sec + 28, g.partStart);                      // hidden sectors
  sec[36] = 0x80;                                    // drive number
  sec[38] = 0x29;                                    // extended boot signature
  put32(sec + 39, volumeId);

  char lab[12];
  snprintf(lab, sizeof(lab), "%-11.11s", label ? label : "USBSTOR");
  memcpy(sec + 43, lab, 11);
  memcpy(sec + 54, "FAT16   ", 8);

  sec[510] = 0x55;
  sec[511] = 0xAA;
}

// Writes `count` zero sectors, reporting progress against a running total.
static bool writeZeroSectors(File &f, uint32_t count, uint8_t *chunk,
                             ProvisionProgress progress, uint32_t *done,
                             uint32_t total) {
  memset(chunk, 0, CHUNK_SECTORS * 512);
  while (count) {
    uint32_t n = count > CHUNK_SECTORS ? CHUNK_SECTORS : count;
    if (f.write(chunk, n * 512) != n * 512) return false;
    count -= n;
    *done += n;
    if (progress && (*done % 2048) < n) {
      progress("Creating image", (uint8_t)((uint64_t)*done * 100 / total));
    }
  }
  return true;
}

bool provisionCreateImage(const String &path, uint32_t sizeMiB,
                          const char *volumeLabel,
                          ProvisionProgress progress) {
  if (sizeMiB < 8) sizeMiB = 8;
  if (sizeMiB > 1024) sizeMiB = 1024;  // FAT16 with sane cluster sizes

  uint32_t totalSectors = sizeMiB * 2048;  // MiB -> 512-byte sectors
  Fat16Geometry g;
  if (!computeGeometry(totalSectors, &g)) {
    log_e("cannot lay out FAT16 for %u MiB", (unsigned)sizeMiB);
    return false;
  }

  log_i("creating %s: %u MiB, spc=%u, fat=%u sectors, %u clusters",
        path.c_str(), (unsigned)sizeMiB, (unsigned)g.sectorsPerCluster,
        (unsigned)g.fatSectors, (unsigned)g.clusters);

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    log_e("cannot create %s", path.c_str());
    return false;
  }

  uint8_t *chunk = (uint8_t *)malloc(CHUNK_SECTORS * 512);
  if (!chunk) {
    f.close();
    return false;
  }

  bool ok = true;
  uint32_t done = 0;
  uint8_t sector[512];

  // MBR, then the gap up to the partition start.
  buildMbr(sector, g);
  ok = ok && f.write(sector, 512) == 512;
  done++;
  ok = ok && writeZeroSectors(f, g.partStart - 1, chunk, progress, &done,
                              totalSectors);

  // Volume boot record.
  if (ok) {
    buildBootSector(sector, g, volumeLabel, (uint32_t)esp_random());
    ok = f.write(sector, 512) == 512;
    done++;
  }

  // Both FAT copies: first sector holds the media descriptor and the end-of-
  // chain marker for the reserved entries 0 and 1, the rest is free space.
  for (int i = 0; i < NUM_FATS && ok; i++) {
    memset(sector, 0, 512);
    sector[0] = 0xF8; sector[1] = 0xFF; sector[2] = 0xFF; sector[3] = 0xFF;
    ok = f.write(sector, 512) == 512;
    done++;
    ok = ok && writeZeroSectors(f, g.fatSectors - 1, chunk, progress, &done,
                                totalSectors);
  }

  // Root directory: a single volume-label entry, then empty slots.
  if (ok) {
    memset(sector, 0, 512);
    char lab[12];
    snprintf(lab, sizeof(lab), "%-11.11s", volumeLabel ? volumeLabel : "USBSTOR");
    memcpy(sector, lab, 11);
    sector[11] = 0x08;  // ATTR_VOLUME_ID
    ok = f.write(sector, 512) == 512;
    done++;
    ok = ok && writeZeroSectors(f, g.rootSectors - 1, chunk, progress, &done,
                                totalSectors);
  }

  // Data area.
  if (ok) {
    uint32_t remaining = totalSectors - done;
    ok = writeZeroSectors(f, remaining, chunk, progress, &done, totalSectors);
  }

  free(chunk);
  f.flush();
  uint32_t finalSize = f.size();
  f.close();

  if (!ok || finalSize != totalSectors * 512ULL) {
    log_e("image creation failed (wrote %u of %u bytes)", (unsigned)finalSize,
          (unsigned)(totalSectors * 512));
    SD_MMC.remove(path);
    return false;
  }

  if (progress) progress("Creating image", 100);
  return true;
}
