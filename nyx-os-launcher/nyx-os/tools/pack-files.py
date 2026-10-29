#!/usr/bin/env python3
"""
pack-files.py — Pack host files into nyx-os/disk.img

Writes files into the Nyx OS disk image using the exact NYXFS002
(auto-grow) on-disk format from cpu/fs.c so they're available when
you boot the OS.

Usage:
    python3 tools/pack-files.py path/to/file1 path/to/file2 ...

Examples:
    python3 tools/pack-files.py ~/Documents/report.pdf
    python3 tools/pack-files.py notes.pdf memo.pdf readme.txt

Layout (from include/fs.h, v7 NYXFS002):
    sector   0       superblock: magic 'NYXFS002' + file_count[4] + frontier[4]
    sectors  1-11    directory:  MAX_FILES entries x 44 bytes
                      (name[32] + used[4] + size[4] + start_lba[4])
    sector   12      desktop icon-layout sector (zeroed — desktop.c owns it)
    sectors  13..    file data:  packed at the frontier, each file's
                      start sector recorded in its directory entry

The image is exactly as big as the data written (frontier sectors) —
it is NOT pre-sized to a theoretical maximum, so capacity scales by
simply writing further into the image (auto-grow).
"""

import struct, sys, os

# ── Constants (must match include/fs.h exactly) ──────────────────
MAGIC              = b'NYXFS002'
MAX_FILES          = 127
MAX_FILENAME       = 32
MAX_FILESIZE       = 16777216  # 16 MiB — matches include/fs.h (dynamic alloc)
SECTOR_SIZE        = 512
DIR_ENTRY_SZ       = 44   # name[32] + used[4] + size[4] + start_lba[4]
DIR_SECTORS        = (MAX_FILES * DIR_ENTRY_SZ + 511) // SECTOR_SIZE   # 11
ICON_LAYOUT_SECTOR = 1 + DIR_SECTORS                                   # 12
DATA_START         = ICON_LAYOUT_SECTOR + 1                            # 13

DISK_IMG = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                        'disk.img')


def main():
    args = sys.argv[1:]
    out = DISK_IMG
    # Optional --out <path> overrides the default output image.
    if '--out' in args:
        i = args.index('--out')
        if i + 1 < len(args):
            out = args[i + 1]
            del args[i:i + 2]
        else:
            print('  ✗  --out requires a path'); sys.exit(1)
    if not args or args[0] in ('-h', '--help'):
        print(__doc__)
        sys.exit(0 if not args else 1)

    file_paths = args

    # ── 1. Read & validate input files ──────────────────────────
    entries = []                # list of (name_str, content_bytes)
    seen_names = set()

    for path in file_paths:
        if len(entries) >= MAX_FILES:
            print(f"  ⚠  Max {MAX_FILES} files, ignoring '{path}'")
            continue

        name = os.path.basename(path)
        if len(name) > MAX_FILENAME - 1:
            print(f"  ⚠  Truncating '{name}' to {MAX_FILENAME - 1} chars")
            name = name[:MAX_FILENAME - 1]

        if name in seen_names:
            print(f"  ⚠  Duplicate name '{name}' — second entry overwrites first")
        seen_names.add(name)

        try:
            with open(path, 'rb') as fh:
                raw = fh.read()
        except OSError as e:
            print(f"  ✗  Can't read '{path}': {e}")
            sys.exit(1)

        if len(raw) > MAX_FILESIZE:
            print(f"  ⚠  '{name}' is {len(raw)} bytes — truncating to {MAX_FILESIZE}")

        entries.append((name, raw[:MAX_FILESIZE]))

    # ── 2. Compute placement: pack files at the frontier ───────
    # frontier = next free sector.  Files are placed one after
    # another starting at DATA_START; the directory entry records
    # each file's absolute start sector + length.
    placement = []              # (start_sector, nsec)
    frontier = DATA_START
    for _name, content in entries:
        nsec = (len(content) + SECTOR_SIZE - 1) // SECTOR_SIZE
        placement.append((frontier, nsec))
        frontier += nsec

    # ── 3. Build superblock ─────────────────────────────────────
    sb = bytearray(SECTOR_SIZE)
    for j, b in enumerate(MAGIC):
        sb[j] = b
    struct.pack_into('<I', sb, 8, len(entries))    # file_count
    struct.pack_into('<I', sb, 12, frontier)       # next free sector

    # ── 4. Build directory (DIR_SECTORS sectors) ────
    dirbuf = bytearray(SECTOR_SIZE * DIR_SECTORS)   # all zeros = unused entries

    for i, (name, content) in enumerate(entries):
        off = i * DIR_ENTRY_SZ
        name_b = name.encode('utf-8', errors='replace')[:MAX_FILENAME - 1]
        for j, b in enumerate(name_b):
            dirbuf[off + j] = b
        struct.pack_into('<I', dirbuf, off + 32, 1)                 # used flag
        struct.pack_into('<I', dirbuf, off + 36, len(content))      # size
        struct.pack_into('<I', dirbuf, off + 40, placement[i][0])   # start_lba

    # ── 5. Build data sectors, packed at the frontier ──────────
    data_blob = bytearray()
    for i, (_name, content) in enumerate(entries):
        for s in range(placement[i][1]):
            offset = s * SECTOR_SIZE
            chunk = content[offset:offset + SECTOR_SIZE]
            data_blob += chunk.ljust(SECTOR_SIZE, b'\0')

    # ── 6. Write the image (exactly frontier sectors — auto-grow) ─
    # Sector 12 (icon layout) is skipped: it sits between the
    # directory and the data frontier and stays zeroed for a fresh
    # pack — desktop.c writes it at runtime.
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)

    with open(out, 'wb') as f:
        f.write(sb)                                     # sector 0
        for s in range(DIR_SECTORS):
            f.write(dirbuf[s*SECTOR_SIZE:(s+1)*SECTOR_SIZE])
        # sector ICON_LAYOUT_SECTOR — leave zeroed
        if ICON_LAYOUT_SECTOR > DIR_SECTORS:
            f.write(b'\0' * SECTOR_SIZE)
        f.write(data_blob)

    # ── 7. Print summary ────────────────────────────────────────
    img_size = os.path.getsize(out)
    total_sectors = frontier   # = 1 + DIR_SECTORS + 1 (icon) + data
    print(f"  ✔  Wrote {len(entries)} file(s) to {out}  ({img_size} bytes)")
    for i, (name, content) in enumerate(entries):
        print(f"     {name:32s} {len(content):4d} bytes  @sector {placement[i][0]}")

    print()
    print("     Layout (auto-grow — image is exactly as big as the data):")
    print("     ───────────────────────────────────────────────────────────")
    print("     sector   0       superblock           512 B")
    print(f"     sectors  1-{DIR_SECTORS}     directory       {DIR_SECTORS*SECTOR_SIZE} B")
    print(f"     sector   {ICON_LAYOUT_SECTOR}       icon layout (desktop)  512 B")
    print(f"     sectors  {DATA_START}-{total_sectors-1:2d}  file data  (frontier-packed)")
    print(f"     Total: {total_sectors} sectors = {img_size} B  → grows on demand in QEMU")


if __name__ == '__main__':
    main()
