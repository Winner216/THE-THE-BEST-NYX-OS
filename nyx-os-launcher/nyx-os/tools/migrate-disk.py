#!/usr/bin/env python3
"""
migrate-disk.py — One-time upgrade of an existing Nyx OS disk image.

The on-disk format changed twice:
  v1 (NYXFS001, 31-slot):  MAX_FILES=31, 40-byte entries, DIR_SECTORS=3,
       DATA_START=4, SECTORS_PER_FILE=1024, ICON_LAYOUT=31748
  v2 (NYXFS001, 127-slot): MAX_FILES=127, 40-byte entries, DIR_SECTORS=10,
       DATA_START=11, SECTORS_PER_FILE=2048, ICON_LAYOUT=260107
  v3 (NYXFS002, auto-grow): MAX_FILES=127, 44-byte entries, DIR_SECTORS=11,
       DATA_START=13 (frontier-packed), ICON_LAYOUT=12

This tool detects which old layout the image uses, reads out every
file's name + content, and re-packs it into the compact NYXFS002
format — preserving the desktop icon-layout sector too.  Files are
packed at the frontier starting at sector 13, so the resulting image
is exactly as big as the data (auto-grow).

Usage:
    python3 tools/migrate-disk.py [disk.img]

Run BEFORE booting a kernel built with the new constants.  A backup of
the old image is written to <disk>.pre-migrate.
"""

import os
import shutil
import struct
import sys

SECTOR = 512

# ── Old layouts (40-byte entries, fixed per-slot regions) ─────────
OLD_MAGIC   = b'NYXFS001'
OLD_ENTRY   = 40

# 31-slot variant
O31_MAX_FILES   = 31
O31_DIR_SECTORS = 3
O31_DATA_START  = 1 + O31_DIR_SECTORS              # 4
O31_SPF         = 1024
O31_ICON        = O31_DATA_START + O31_MAX_FILES * O31_SPF   # 31748

# 127-slot variant
O127_MAX_FILES   = 127
O127_DIR_SECTORS = 10
O127_DATA_START  = 1 + O127_DIR_SECTORS            # 11
O127_SPF         = 2048
O127_ICON        = O127_DATA_START + O127_MAX_FILES * O127_SPF  # 260107

# ── New layout (must match include/fs.h / cpu/fs.c) ──────────────
NEW_MAGIC          = b'NYXFS002'
NEW_MAX_FILES      = 127
NEW_MAX_FILESIZE   = 3145728
NEW_ENTRY          = 44
NEW_DIR_SECTORS    = (NEW_MAX_FILES * NEW_ENTRY + 511) // SECTOR   # 11
NEW_ICON_LAYOUT    = 1 + NEW_DIR_SECTORS                            # 12
NEW_DATA_START     = NEW_ICON_LAYOUT + 1                            # 13


def get_u32(buf, off):
    return struct.unpack('<I', buf[off:off + 4])[0]


def put_u32(buf, off, v):
    struct.pack_into('<I', buf, off, v)


def read_old_entries(old, max_files, dir_sectors, data_start, spf, max_size):
    """Read all files from an old-layout image. Returns (entries, file_count)."""
    entries = []            # (name_bytes, content_bytes) — slot order, None if unused
    for i in range(max_files):
        off = SECTOR + i * OLD_ENTRY
        name = old[off:off + 32].split(b'\0', 1)[0]
        used = get_u32(old, off + 32)
        clen = get_u32(old, off + 36)
        if not used or clen <= 0:
            entries.append(None)
            continue
        if clen > max_size:
            clen = max_size
        base = (data_start + i * spf) * SECTOR
        nsec = (clen + SECTOR - 1) // SECTOR
        content = bytearray()
        for s in range(nsec):
            sec_off = base + s * SECTOR
            if sec_off >= len(old):
                break
            content += old[sec_off:sec_off + SECTOR]
        entries.append((name, bytes(content[:clen])))
    return entries


def main():
    if len(sys.argv) > 2 or sys.argv[1] in ('-h', '--help'):
        print(__doc__)
        return 1 if len(sys.argv) > 1 else 0

    disk = sys.argv[1] if len(sys.argv) == 2 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'disk.img')

    with open(disk, 'rb') as f:
        old = f.read()
    old_sectors = len(old) // SECTOR

    if old[:8] != OLD_MAGIC:
        # Maybe already new format — nothing to do.
        if old[:8] == NEW_MAGIC:
            print(f"  ✔  {disk} is already NYXFS002 — no migration needed")
            return 0
        print(f"  ✗  {disk} has no NYXFS001 superblock — nothing to migrate")
        return 1

    # ── Detect which old variant by checking the icon-layout magic ──
    if old[O31_ICON * SECTOR:O31_ICON * SECTOR + 4] == b'NYIX':
        max_files, dir_sectors, data_start, spf, old_icon = \
            O31_MAX_FILES, O31_DIR_SECTORS, O31_DATA_START, O31_SPF, O31_ICON
        print("  ℹ  Detected old 31-slot layout (NYXFS001)")
    elif old[O127_ICON * SECTOR:O127_ICON * SECTOR + 4] == b'NYIX' or old_sectors > 100000:
        max_files, dir_sectors, data_start, spf, old_icon = \
            O127_MAX_FILES, O127_DIR_SECTORS, O127_DATA_START, O127_SPF, O127_ICON
        print("  ℹ  Detected old 127-slot layout (NYXFS001)")
    else:
        max_files, dir_sectors, data_start, spf, old_icon = \
            O31_MAX_FILES, O31_DIR_SECTORS, O31_DATA_START, O31_SPF, O31_ICON
        print("  ℹ  Detected old 31-slot layout (NYXFS001, by size)")

    file_count = get_u32(old, 8)

    entries = read_old_entries(old, max_files, dir_sectors, data_start,
                               spf, NEW_MAX_FILESIZE)
    used = [e for e in entries if e is not None]
    print(f"  ℹ  Old image: {old_sectors} sectors, {len(used)} used file(s), "
          f"stored count {file_count}")

    # Old icon-layout sector (may not exist on a fresh disk)
    icon_old = old[old_icon * SECTOR:old_icon * SECTOR + SECTOR]
    has_layout = len(icon_old) == SECTOR and icon_old[:4] == b'NYIX'

    # ── Pack into the new compact layout ─────────────────────────
    # Placement: files packed at the frontier from NEW_DATA_START.
    placement = []          # (start_sector, nsec)
    frontier = NEW_DATA_START
    for e in entries:
        if e is None:
            placement.append(None)
            continue
        nsec = (len(e[1]) + SECTOR - 1) // SECTOR
        placement.append((frontier, nsec))
        frontier += nsec

    total_sectors = frontier
    new = bytearray(total_sectors * SECTOR)

    # superblock: magic + file_count + frontier
    new[:8] = NEW_MAGIC
    put_u32(new, 8, len(used))
    put_u32(new, 12, frontier)

    # directory — 44-byte entries across NEW_DIR_SECTORS
    for i in range(NEW_MAX_FILES):
        off = SECTOR + i * NEW_ENTRY
        if entries[i] is not None:
            name, content = entries[i]
            for j, b in enumerate(name[:31]):
                new[off + j] = b
            put_u32(new, off + 32, 1)
            put_u32(new, off + 36, len(content))
            put_u32(new, off + 40, placement[i][0])
        # else: zeroed = unused entry

    # icon-layout sector → fixed sector NEW_ICON_LAYOUT
    if has_layout:
        new[NEW_ICON_LAYOUT * SECTOR:NEW_ICON_LAYOUT * SECTOR + SECTOR] = icon_old

    # file data — packed at the frontier
    for i in range(NEW_MAX_FILES):
        if entries[i] is None:
            continue
        content = entries[i][1]
        base = placement[i][0] * SECTOR
        new[base:base + len(content)] = content

    # ── Write out ─────────────────────────────────────────────
    backup = disk + '.pre-migrate'
    shutil.copyfile(disk, backup)
    with open(disk, 'wb') as f:
        f.write(new)

    print(f"  ✔  Backed up old image to {backup}")
    print(f"  ✔  Migrated {disk}:")
    print(f"       old: {old_sectors} sectors ({len(old)} bytes)")
    print(f"       new: {total_sectors} sectors ({len(new)} bytes) — compact, auto-grows")
    print(f"       files preserved: {len(used)}")
    print(f"       icon layout {'migrated' if has_layout else 'not present (fresh disk)'} "
          f"(→ sector {NEW_ICON_LAYOUT})")
    return 0


if __name__ == '__main__':
    sys.exit(main())
