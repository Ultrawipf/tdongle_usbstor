#!/usr/bin/env python3
"""Build a FAT disk image from a folder, for usb_stor_emu.

The dongle serves the raw sectors of an image file, so the content pipeline is
"folder -> image -> SD card". This script does the first step without needing
admin rights or loop mounts: it writes an MBR plus a FAT16 or FAT32 volume and
populates it directly.

Usage:
    python make_image.py content/ images/disk1.img --size 64 --label MYDISK
    python make_image.py --blank images/empty.img --size 256

Sizes are MiB. FAT16 is used below 512 MiB, FAT32 at or above it.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from datetime import datetime
from pathlib import Path

SECTOR = 512
PART_START = 2048  # 1 MiB aligned
RESERVED_FAT16 = 1
RESERVED_FAT32 = 32
NUM_FATS = 2
ROOT_ENTRIES_16 = 512


class FatBuilder:
    """Minimal FAT16/FAT32 writer.

    Only what an image builder needs: create directories, add files, set a
    volume label. No deletion, no fragmentation handling beyond sequential
    allocation, which is all a freshly built image requires.
    """

    def __init__(self, total_sectors: int, label: str):
        self.part_start = PART_START
        self.part_sectors = total_sectors - PART_START
        self.label = label[:11].upper().ljust(11)
        self.total_sectors = total_sectors

        self.fat32 = self.part_sectors >= 512 * 2048
        self._layout()

        # Cluster 0/1 are reserved; entry i holds the next cluster in a chain.
        self.fat: list[int] = [0] * (self.clusters + 2)
        self.fat[0] = 0x0FFFFFF8 if self.fat32 else 0xFFF8
        self.fat[1] = 0x0FFFFFFF if self.fat32 else 0xFFFF
        self.next_free = 3 if self.fat32 else 2

        self.cluster_data: dict[int, bytes] = {}
        self.root_entries: list[bytes] = []

        if self.fat32:
            self.root_cluster = 2
            self.fat[2] = 0x0FFFFFFF

    def _layout(self) -> None:
        if self.fat32:
            # 4 KiB clusters, matching what Windows picks for volumes this
            # size. Starting at 1 would technically work but produces a
            # needlessly huge FAT.
            spc = 8
            while spc < 128 and (self.part_sectors // spc) > 4177918:
                spc *= 2
            # Keep clusters comfortably above the FAT32 minimum of 65525.
            while spc > 1 and (self.part_sectors // spc) < 70000:
                spc //= 2
            self.spc = spc
            self.reserved = RESERVED_FAT32
            self.root_sectors = 0
            fat_sectors = 1
            for _ in range(16):
                data = self.part_sectors - self.reserved - NUM_FATS * fat_sectors
                clusters = data // spc
                need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
                if need == fat_sectors:
                    break
                fat_sectors = need
        else:
            spc = 4
            while spc < 128 and (self.part_sectors // spc) > 65524:
                spc *= 2
            self.spc = spc
            self.reserved = RESERVED_FAT16
            self.root_sectors = (ROOT_ENTRIES_16 * 32 + SECTOR - 1) // SECTOR
            fat_sectors = 1
            for _ in range(16):
                data = (self.part_sectors - self.reserved - self.root_sectors
                        - NUM_FATS * fat_sectors)
                clusters = data // spc
                need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
                if need == fat_sectors:
                    break
                fat_sectors = need

        self.fat_sectors = fat_sectors
        overhead = self.reserved + self.root_sectors + NUM_FATS * fat_sectors
        self.clusters = (self.part_sectors - overhead) // self.spc
        self.data_start = self.part_start + overhead

        if self.fat32 and self.clusters < 65525:
            raise SystemExit("volume too small for FAT32; use a larger --size")
        if not self.fat32 and not (4085 <= self.clusters <= 65524):
            raise SystemExit("volume size not addressable as FAT16")

    # -- allocation ------------------------------------------------------

    def _alloc_chain(self, data: bytes) -> int:
        """Stores `data` in a fresh cluster chain, returns the first cluster."""
        size = self.spc * SECTOR
        chunks = [data[i:i + size] for i in range(0, len(data), size)] or [b""]
        first = None
        prev = None
        for chunk in chunks:
            if self.next_free >= self.clusters + 2:
                raise SystemExit("image is full; increase --size")
            cur = self.next_free
            self.next_free += 1
            self.cluster_data[cur] = chunk.ljust(size, b"\0")
            self.fat[cur] = 0x0FFFFFFF if self.fat32 else 0xFFFF
            if prev is not None:
                self.fat[prev] = cur
            if first is None:
                first = cur
            prev = cur
        return first

    # -- directory entries -----------------------------------------------

    @staticmethod
    def _short_name(name: str, used: set[str]) -> bytes:
        stem, _, ext = name.rpartition(".")
        if not stem:
            stem, ext = name, ""

        def clean(s: str, n: int) -> str:
            out = "".join(c if (c.isalnum() or c in "$%'-_@~`!(){}^#&") else "_"
                          for c in s.upper())
            return out[:n]

        base, ext = clean(stem, 8), clean(ext, 3)
        candidate = (base.ljust(8) + ext.ljust(3))
        if candidate in used:
            # Standard ~N disambiguation.
            for i in range(1, 1000):
                suffix = f"~{i}"
                trial = (base[:8 - len(suffix)] + suffix).ljust(8) + ext.ljust(3)
                if trial not in used:
                    candidate = trial
                    break
        used.add(candidate)
        return candidate.encode("ascii", "replace")

    @staticmethod
    def _fat_time(ts: float) -> tuple[int, int]:
        dt = datetime.fromtimestamp(ts)
        year = max(dt.year, 1980)
        date = ((year - 1980) << 9) | (dt.month << 5) | dt.day
        time = (dt.hour << 11) | (dt.minute << 5) | (dt.second // 2)
        return date, time

    def _entry(self, name83: bytes, attr: int, cluster: int, size: int,
               ts: float) -> bytes:
        date, time = self._fat_time(ts)
        return struct.pack(
            "<11sBBBHHHHHHHL",
            name83, attr, 0, 0, time, date, date, (cluster >> 16) & 0xFFFF,
            time, date, cluster & 0xFFFF, size,
        )

    def add_tree(self, src: Path) -> None:
        """Adds the contents of `src` (not src itself) at the volume root."""
        used: set[str] = set()
        entries = self._build_dir(src, used, parent_cluster=0, is_root=True)
        if self.fat32:
            self._write_dir_cluster_chain(self.root_cluster, entries)
        else:
            self.root_entries = entries

    def _build_dir(self, path: Path, used: set[str], parent_cluster: int,
                   is_root: bool) -> list[bytes]:
        entries: list[bytes] = []
        if is_root:
            entries.append(self._entry(self.label.encode("ascii", "replace"),
                                       0x08, 0, 0, datetime.now().timestamp()))
        else:
            now = datetime.now().timestamp()
            entries.append(self._entry(b".          ", 0x10, self._cur, 0, now))
            entries.append(self._entry(b"..         ", 0x10, parent_cluster, 0, now))

        for child in sorted(path.iterdir(), key=lambda p: p.name.lower()):
            local_used: set[str] = set()
            if child.is_dir():
                cluster = self._reserve_dir_cluster()
                saved = getattr(self, "_cur", 0)
                self._cur = cluster
                sub = self._build_dir(child, local_used, parent_cluster,
                                      is_root=False)
                self._cur = saved
                self._write_dir_cluster_chain(cluster, sub)
                entries.append(self._entry(
                    self._short_name(child.name, used), 0x10, cluster, 0,
                    child.stat().st_mtime))
            elif child.is_file():
                data = child.read_bytes()
                cluster = self._alloc_chain(data) if data else 0
                entries.append(self._entry(
                    self._short_name(child.name, used), 0x20, cluster,
                    len(data), child.stat().st_mtime))
        return entries

    def _reserve_dir_cluster(self) -> int:
        if self.next_free >= self.clusters + 2:
            raise SystemExit("image is full; increase --size")
        cluster = self.next_free
        self.next_free += 1
        self.fat[cluster] = 0x0FFFFFFF if self.fat32 else 0xFFFF
        self.cluster_data[cluster] = b"\0" * (self.spc * SECTOR)
        return cluster

    def _write_dir_cluster_chain(self, first: int, entries: list[bytes]) -> None:
        blob = b"".join(entries)
        size = self.spc * SECTOR
        chunks = [blob[i:i + size] for i in range(0, len(blob), size)] or [b""]
        cur = first
        for i, chunk in enumerate(chunks):
            self.cluster_data[cur] = chunk.ljust(size, b"\0")
            if i + 1 < len(chunks):
                nxt = self._reserve_dir_cluster()
                self.fat[cur] = nxt
                cur = nxt
            else:
                self.fat[cur] = 0x0FFFFFFF if self.fat32 else 0xFFFF

    # -- output ----------------------------------------------------------

    def _boot_sector(self) -> bytes:
        s = bytearray(SECTOR)
        s[0:3] = b"\xEB\x58\x90" if self.fat32 else b"\xEB\x3C\x90"
        s[3:11] = b"MSDOS5.0"
        struct.pack_into("<H", s, 11, SECTOR)
        s[13] = self.spc
        struct.pack_into("<H", s, 14, self.reserved)
        s[16] = NUM_FATS
        struct.pack_into("<H", s, 17, 0 if self.fat32 else ROOT_ENTRIES_16)
        if self.part_sectors < 0x10000 and not self.fat32:
            struct.pack_into("<H", s, 19, self.part_sectors)
        else:
            struct.pack_into("<H", s, 19, 0)
            struct.pack_into("<L", s, 32, self.part_sectors)
        s[21] = 0xF8
        struct.pack_into("<H", s, 22, 0 if self.fat32 else self.fat_sectors)
        struct.pack_into("<H", s, 24, 63)
        struct.pack_into("<H", s, 26, 255)
        struct.pack_into("<L", s, 28, self.part_start)

        if self.fat32:
            struct.pack_into("<L", s, 36, self.fat_sectors)
            struct.pack_into("<H", s, 40, 0)
            struct.pack_into("<H", s, 42, 0)
            struct.pack_into("<L", s, 44, self.root_cluster)
            struct.pack_into("<H", s, 48, 1)   # FSInfo sector
            struct.pack_into("<H", s, 50, 6)   # backup boot sector
            s[64] = 0x80
            s[66] = 0x29
            struct.pack_into("<L", s, 67, 0x12345678)
            s[71:82] = self.label.encode("ascii", "replace")
            s[82:90] = b"FAT32   "
        else:
            s[36] = 0x80
            s[38] = 0x29
            struct.pack_into("<L", s, 39, 0x12345678)
            s[43:54] = self.label.encode("ascii", "replace")
            s[54:62] = b"FAT16   "

        s[510] = 0x55
        s[511] = 0xAA
        return bytes(s)

    def _mbr(self) -> bytes:
        s = bytearray(SECTOR)
        ptype = 0x0C if self.fat32 else 0x0E  # LBA variants
        e = 446
        s[e] = 0x00
        s[e + 1:e + 4] = b"\xFE\xFF\xFF"
        s[e + 4] = ptype
        s[e + 5:e + 8] = b"\xFE\xFF\xFF"
        struct.pack_into("<L", s, e + 8, self.part_start)
        struct.pack_into("<L", s, e + 12, self.part_sectors)
        s[510] = 0x55
        s[511] = 0xAA
        return bytes(s)

    def _fat_bytes(self) -> bytes:
        if self.fat32:
            raw = b"".join(struct.pack("<L", v & 0x0FFFFFFF) for v in self.fat)
        else:
            raw = b"".join(struct.pack("<H", v & 0xFFFF) for v in self.fat)
        return raw.ljust(self.fat_sectors * SECTOR, b"\0")

    def write(self, out: Path) -> None:
        with out.open("wb") as f:
            f.write(self._mbr())
            f.write(b"\0" * ((self.part_start - 1) * SECTOR))

            f.write(self._boot_sector())
            if self.fat32:
                fsinfo = bytearray(SECTOR)
                struct.pack_into("<L", fsinfo, 0, 0x41615252)
                struct.pack_into("<L", fsinfo, 484, 0x61417272)
                free = self.clusters - (self.next_free - 2)
                struct.pack_into("<L", fsinfo, 488, free)
                struct.pack_into("<L", fsinfo, 492, self.next_free)
                fsinfo[510] = 0x55
                fsinfo[511] = 0xAA
                f.write(bytes(fsinfo))
                f.write(b"\0" * (4 * SECTOR))
                f.write(self._boot_sector())        # backup at sector 6
                f.write(b"\0" * ((self.reserved - 7) * SECTOR))
            f.write(b"" if self.fat32 else b"")

            fat = self._fat_bytes()
            for _ in range(NUM_FATS):
                f.write(fat)

            if not self.fat32:
                root = b"".join(self.root_entries)
                f.write(root.ljust(self.root_sectors * SECTOR, b"\0"))

            blank = b"\0" * (self.spc * SECTOR)
            first = 2
            for c in range(first, self.clusters + 2):
                f.write(self.cluster_data.get(c, blank))

            # Pad to the exact requested size.
            f.truncate(self.total_sectors * SECTOR)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", nargs="?", help="folder whose contents fill the image")
    ap.add_argument("output", help="image file to write")
    ap.add_argument("--size", type=int, default=64, help="image size in MiB")
    ap.add_argument("--label", default="USBSTOR", help="volume label (11 chars)")
    ap.add_argument("--blank", action="store_true", help="create an empty image")
    args = ap.parse_args()

    if not args.blank and not args.source:
        ap.error("give a source folder, or pass --blank")

    total_sectors = args.size * 2048
    builder = FatBuilder(total_sectors, args.label)

    if not args.blank:
        src = Path(args.source)
        if not src.is_dir():
            print(f"error: {src} is not a directory", file=sys.stderr)
            return 1
        builder.add_tree(src)

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    builder.write(out)

    kind = "FAT32" if builder.fat32 else "FAT16"
    print(f"wrote {out} ({args.size} MiB, {kind}, {builder.spc * SECTOR}B clusters, "
          f"{builder.clusters} clusters, label {args.label})")
    print("Copy it into /images on the dongle's SD card.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
