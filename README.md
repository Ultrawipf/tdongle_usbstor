# usb_stor_emu

Turn a **LilyGO T-Dongle-S3** into a USB mass storage device whose USB identity,
capacity and content are defined by a config file on its microSD card.

The dongle serves a **disk image file** from the SD card. The host owns the
filesystem inside that image, so reads and writes behave exactly like a real
USB stick — and only the selected image is visible, never the rest of the card.

## Features

- Configurable vendor ID, product ID, vendor name, product name and serial
- Configurable SCSI inquiry strings
- Read-only mode, enforced with a real write-protect bit
- Multiple images on one card, switchable by config file or by button
- Per-image USB identity — each image can present itself as a different device
- Reported capacity can be reduced independently of the image size
- Maintenance mode exposes the whole SD card over USB for config edits
- Status LED for connection and read/write activity
- Status display showing the active image, size, mode and activity
- Creates a working config and image automatically on a blank card

## Quick start

1. Flash the firmware (see [Building](#building)).
2. Insert a microSD card and plug the dongle in.
3. On a blank card the firmware creates `/usbstor.ini` and a 64 MiB
   `/images/disk1.img`, then presents it as a USB stick. This takes about
   30 seconds, shown on the display.
4. Edit `/usbstor.ini` to change the USB identity, then replug.

## Putting content on the device

USB mass storage is a block device, so content is packaged as a disk image:

```
python tools/make_image.py content/ disk1.img --size 64 --label MYDISK
```

| Option | Meaning |
| --- | --- |
| `--size` | image size in MiB (default 64) |
| `--label` | volume label, up to 11 characters |
| `--blank` | create an empty image instead of copying a folder |

FAT16 is used below 512 MiB and FAT32 at or above it. No admin rights needed.

Copy the resulting image into `/images` on the SD card, either with a card
reader or through [maintenance mode](#maintenance-mode).

## SD card layout

```
/usbstor.ini          configuration
/images/disk1.img     one or more *.img / *.bin / *.iso / *.dsk files
```

## Configuration

All settings live in `/usbstor.ini`. Numbers accept decimal or `0x` hex, and
`;` or `#` start a comment.

```ini
[usb]
vid      = 0x303A
pid      = 0x4002
vendor   = LilyGO
product  = T-Dongle-S3 Disk
serial   =                  ; empty -> derived from the chip MAC
readonly = false            ; true -> host mounts the volume write-protected

scsi_vendor   = LILYGO      ; SCSI inquiry strings, max 8 / 16 / 4 characters
scsi_product  = FlexDisk
scsi_revision = 1.0

capacity_mib  = 0           ; report a smaller capacity; 0 = full image size

[content]
dir    = /images            ; directory scanned for images
active = disk1.img          ; image to serve; button selection is saved here
default_size_mib = 64       ; size of the image created on a blank card

[display]
enabled   = true
backlight = 200             ; 0..255
title     =                 ; empty -> falls back to the product name

[led]
enabled    = true
brightness = 8              ; 0..31
```

### Per-image settings

An `[image:<file name>]` section overrides any `[usb]` or `[display]` setting
for that image alone. Anything not listed falls back to `[usb]`, and an image
without a section uses `[usb]` unchanged. Section names are matched
case-insensitively against the file name.

```ini
[image:secret.img]
product  = Secure Volume
vid      = 0x1234
pid      = 0x5678
readonly = true
title    = SECURE
```

This lets every image appear as a completely different device.

### Maintenance settings

An optional `[maintenance]` section overrides the identity used in maintenance
mode, accepting the same keys as `[usb]`.

```ini
[maintenance]
product = Dongle Service Mode
pid     = 0x4010
```

### Limits

`vendor`, `product` and `serial` accept up to **125 characters**. This is a USB
protocol ceiling: a string descriptor's length field is a single byte and the
text is stored as UTF-16. Longer values are truncated.

`capacity_mib` can only reduce the reported size below the real image size. A
larger value is ignored, so the host is never offered space that does not exist.

The SCSI inquiry strings are limited to 8, 16 and 4 characters respectively by
the SCSI standard.

## Controls

The dongle has a single button, **BOOT**, which is used while the device is
running. Holding it during power-up does *not* reach the firmware — that
combination is reserved by the chip for its own boot loader.

| Action | Result |
| --- | --- |
| Long press (hold ~1 s) | open the selector |
| Short press in selector | move to the next entry |
| Long press in selector | confirm the current entry |
| No input for 15 s | confirm the current entry |

The selector cycles through every image on the card plus a final
`MAINTENANCE` entry. Choosing a different image saves it as the active one and
restarts; choosing `MAINTENANCE` enters maintenance mode.

Each change restarts the dongle, because the USB identity and capacity are
fixed while the device is connected.

## Maintenance mode

Normally only the selected image is visible to the host, which keeps the rest
of the card private. Maintenance mode temporarily exposes the **entire SD card**
instead, so the config can be edited and new images added over USB without
removing the card.

Enter it from the selector, and leave it with a long press. It appears as a
separate device:

| | Normal | Maintenance |
| --- | --- | --- |
| Product ID | `pid` | `pid + 1` |
| Product name | `product` | `product` + ` SD` |
| Content | the selected image | the whole card |
| Write access | as configured | always writable |

Maintenance mode never survives unplugging the dongle, so a power cycle always
returns it to normal operation.

## Status indicators

**LED**

| Colour | Meaning |
| --- | --- |
| Red | no SD card or no image found |
| Amber | ready, waiting for a host |
| Green | host connected |
| Blue flash | read activity |
| Magenta flash | write activity |
| Cyan | selector open |
| White | maintenance mode |

**Display** shows the title, the active image, its size, read-only or
read/write, and the current connection or activity state.

## Building

Requires [PlatformIO](https://platformio.org/). From the project directory:

```
pio run                       # build
pio run -t upload             # build and flash
```

The dongle also provides a USB serial console at 115200 baud, which reports the
active configuration and any problems with the card or config file.

### Entering the boot loader

Once the firmware is running, the dongle presents itself as a mass storage
device rather than a programmer, so flashing needs the boot loader:

- **Automatic** — `pio run -t upload` handles this on most systems.
- **Manual** — unplug the dongle, hold **BOOT**, plug it back in, then release.

## Hardware

Built for the LilyGO T-Dongle-S3 (ESP32-S3, 16 MB flash).

| Function | Pins |
| --- | --- |
| microSD (SDMMC 4-bit) | CLK 12, CMD 16, D0 14, D1 17, D2 21, D3 18 |
| ST7735 LCD 160x80 | MOSI 3, SCLK 5, CS 4, DC 2, RST 1, BL 38 |
| APA102 LED | DI 40, CI 39 |
| Button | BOOT (GPIO 0) |

The `-Plus` and `-Dual` variants have additional pins defined; see the
commented build flags in `platformio.ini`.
