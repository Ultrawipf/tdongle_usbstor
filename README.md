# usb_stor_emu

Turns a **LilyGO T-Dongle-S3** into a USB mass storage device whose USB identity,
capacity and content are defined by a config file on its microSD card.

The dongle serves the raw sectors of a **disk image file** on the SD card. The
host owns the filesystem inside that image, so reads and writes behave exactly
like a real stick — and only the selected image is visible, never the rest of
the card.

## Status

| Requirement | State |
| --- | --- |
| Vendor and product ID | done |
| Vendor name / product name / serial string | done |
| Read-only mode on/off | done (real SCSI write-protect bit) |
| Config file on SD card | done — `/usbstor.ini` |
| Content selectable by config | done — image file, selectable by config or button |
| Only active content visible | done — nothing but the chosen image is exposed |
| Status LED (connection, access) | done — APA102 |
| Capacity *(optional)* | done — `capacity_mib` (shrink only) |
| Custom string IDs *(optional)* | done — SCSI inquiry strings |
| Status display *(optional)* | done — title, image name, size, RO/RW, activity |
| Maintenance mode *(added)* | done — full raw SD passthrough over USB |
| Other T-Dongle variants *(optional)* | pin definitions present, untested |

Verified on hardware: enumerates as `303A:4002 "LILYGO FlexDisk"`, mounts a
64 MiB FAT16 volume, 1 MiB write round-trips byte-identically and survives a
reboot, and `readonly = true` makes Windows report *"The media is write
protected."*

## Content model

MSC is a **block** device, so the unit of content is a disk image, not a folder.
The workflow is `folder -> image -> SD card`:

```
python tools/make_image.py content/ disk1.img --size 64 --label MYDISK
```

Then copy `disk1.img` into `/images` on the SD card. `make_image.py` writes an
MBR plus a FAT16 (<512 MiB) or FAT32 (>=512 MiB) volume and populates it
directly — no admin rights, no loop mounts.

Put several images in `/images` and pick between them with the config file or
the BOOT button.

## SD card layout

```
/usbstor.ini          configuration (auto-created on first run)
/images/disk1.img     one or more *.img / *.bin / *.iso / *.dsk files
```

On a card with no images, the firmware creates `/images/disk1.img` (64 MiB,
formatted FAT16) and a documented `/usbstor.ini`, so a blank card yields a
working stick without a card reader. This takes ~30 s, shown on the display.

## Configuration

`/usbstor.ini`. Numbers accept decimal or `0x` hex; `;` and `#` start comments.

```ini
[usb]
vid      = 0x303A
pid      = 0x4002
vendor   = LilyGO
product  = T-Dongle-S3 Disk
serial   =                  ; empty -> derived from the chip MAC
readonly = false            ; true -> host mounts the volume write-protected

scsi_vendor   = LILYGO      ; SCSI INQUIRY strings, max 8 / 16 / 4 chars
scsi_product  = FlexDisk
scsi_revision = 1.0

capacity_mib  = 0           ; report a smaller capacity; 0 = full image size

[content]
dir    = /images
active = disk1.img          ; button selection is written back here
default_size_mib = 64       ; size of the image created on first run

[display]
enabled   = true
backlight = 200
title     =                 ; empty -> falls back to the product name

[led]
enabled    = true
brightness = 8              ; APA102 global current, 0..31

; Per-image overrides. Section name must match the file name; any key from
; [usb] or [display] is accepted.
[image:secret.img]
product  = Secure Volume
pid      = 0x5678
readonly = true
title    = SECURE

; Optional. Overrides the maintenance-mode identity; same keys as [usb].
[maintenance]
product = Dongle Service Mode
pid     = 0x4010
```

`capacity_mib` only ever **shrinks** the reported size — reporting blocks that
are not backed by the file would hand the host storage that fails on access.

## Controls

**BOOT button — runtime only.** GPIO0 held at reset puts the ROM into download
mode, so the button can never reach this firmware during boot. Every gesture
below is performed while the dongle is running.

- **Long press (>0.8 s)** → selector. Short presses cycle through the images
  plus a final `MAINTENANCE` entry; a long press confirms, and 15 s of
  inactivity confirms the current entry.
- Picking a different image saves it to `usbstor.ini` and reboots.
- Picking `MAINTENANCE` reboots into maintenance mode.
- **Long press in maintenance mode** → back to normal.

Each transition reboots via `usb_persist_restart(RESTART_NO_PERSIST)`, which
resets the USB peripheral and forces the host to re-enumerate. This is
necessary rather than cosmetic: descriptors, SCSI strings and the reported
capacity are all fixed at enumeration time.

## Maintenance mode

Normally only the selected image is exposed, which means `/usbstor.ini` and
`/images/` are unreachable from the host — good for isolation, awkward for
configuration. Maintenance mode solves that by exposing the **entire physical
SD card** instead, partition table included, so the config can be edited and
new images dropped in over USB without opening the dongle.

It presents a deliberately different identity so the host does not reuse cached
geometry from the normal device:

| | Normal | Maintenance |
| --- | --- | --- |
| PID | `pid` | `pid + 1` |
| Product | `product` | `product` + ` SD` |
| SCSI product | `scsi_product` | `SD CARD` |
| Content | selected image | whole card |
| Write access | per `readonly` | always writable |

Override any of it with a `[maintenance]` section, which accepts the same keys
as `[usb]`.

Maintenance mode is held in RTC memory that survives a software restart but not
a power cycle, so unplugging always returns the dongle to normal operation.

Internally this unmounts the FATFS/VFS layer and drives the SDMMC host directly
(`src/rawsd.cpp`), since block access has to sit below the filesystem. Once
entered, no further filesystem access is possible until the next reboot.

**Status LED (APA102)**

| Colour | Meaning |
| --- | --- |
| Red | no SD card / no image |
| Amber | ready, host has not enumerated |
| Green | host connected |
| Blue flash | read access |
| Magenta flash | write access |
| Cyan | image selection mode |
| White | maintenance mode (raw card exposed) |

**Display** shows the title, active image name, size, RO/RW and connection or
access state.

## Building and flashing

Uses the PlatformIO already installed with VS Code:

```
C:\Users\Yannick\.platformio\penv\Scripts\pio.exe run
C:\Users\Yannick\.platformio\penv\Scripts\pio.exe run -t upload --upload-port COMx
```

### USB mode caveat

The stock LilyGO board definition builds with `ARDUINO_USB_MODE=1` (hardware
USB-Serial-JTAG), which cannot do MSC. This project ships its own board
definition in `boards/tdongle-s3-msc.json` with `ARDUINO_USB_MODE=0` (TinyUSB).

`ARDUINO_USB_CDC_ON_BOOT` must stay **0**. With it enabled the Arduino core
calls `USB.begin()` before `setup()` runs, freezing the descriptors before the
config file has been read — the device then enumerates with Espressif's default
`303A:1001` no matter what the config says. The CDC console is instead started
by `src/logbuf.cpp` after the descriptors are set and before `USB.begin()`.

Because the console appears late, everything logged during SD mount, config
parsing and image creation is buffered (4 KB, `esp_log` included) and replayed
when a host opens the port.

### Getting back into the bootloader

Once this firmware runs, the ROM serial port is gone and `esptool` cannot
auto-reset. Two options:

- **1200 baud touch** — opening the CDC port at 1200 baud triggers
  `usb_persist_restart(RESTART_BOOTLOADER)`; the dongle reappears as a
  USB-Serial-JTAG port ready for flashing. This is what the Arduino IDE does.
- **BOOT button** — unplug, hold BOOT, plug back in, release.

## Layout

| Path | Role |
| --- | --- |
| `boards/tdongle-s3-msc.json` | board definition with TinyUSB enabled |
| `src/config.cpp` | INI parser, per-image overrides, active-image write-back |
| `src/storage.cpp` | SD_MMC 4-bit mount, image-file block device (mutex-guarded) |
| `src/rawsd.cpp` | raw SDMMC block access for maintenance mode |
| `src/usb_msc.cpp` | descriptors, SCSI strings, write-protect callback |
| `src/provision.cpp` | first-run config + MBR/FAT16 image creation |
| `src/display.cpp` | dependency-free ST7735 driver and status screens |
| `src/status_led.cpp` | bit-banged APA102 |
| `src/logbuf.cpp` | boot log buffer + USB CDC console |
| `tools/make_image.py` | folder -> FAT16/FAT32 image builder |

### Read-only implementation note

Making a host mount a volume read-only requires the SCSI write-protect bit,
which TinyUSB reads from `tud_msc_is_writable_cb()`. The Arduino core never
defines it, so TinyUSB's weak default ("always writable") applies. `usb_msc.cpp`
defines the symbol itself, overriding the weak default at link time. Writes are
additionally rejected in the storage layer, and a read-only image is opened
without write access at all, so the card cannot be modified even by a host that
ignores the bit.

## Hardware

T-Dongle-S3: ESP32-S3, 16 MB flash, no PSRAM.

| Function | Pins |
| --- | --- |
| microSD (SDMMC 4-bit) | CLK 12, CMD 16, D0 14, D1 17, D2 21, D3 18 |
| ST7735 LCD 160x80 | MOSI 3, SCLK 5, CS 4, DC 2, RST 1, BL 38 |
| APA102 LED | DI 40, CI 39 |
| Button | BOOT (GPIO 0) |

Panel settings follow the vendor factory example: colour inverted, BGR order,
gap x=1 / y=26, landscape via MADCTL `MY|MV`.

For the `-Plus` / `-Dual` variants see the commented `build_flags` in
`platformio.ini`; the extra pins are defined in `src/pins.h` but untested.
