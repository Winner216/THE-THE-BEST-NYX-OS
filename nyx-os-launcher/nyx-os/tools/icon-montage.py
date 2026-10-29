#!/usr/bin/env python3
"""
icon-montage.py — Crop the 19 desktop app-icon tiles out of a QEMU
`-display none` screendump (PPM) and assemble them into a single
labeled montage PNG, saved as tools/icon-baseline.png.

Use case: the desktop renders each app's SVG picture (background-
less glyph on the wallpaper, since the squircle shells were stripped
in the icon-background-removal work).  This baseline lets you eyeball
future icon-style changes against a captured reference without
boot-looping: re-run QEMU, `screendump /tmp/desktop.ppm`, then
`python3 tools/icon-montage.py /tmp/desktop.ppm`.

The grid math below mirrors cpu/desktop.c's compute_icon_grid()
exactly (14 columns, cell_w floored at ICON_TILE_W+8, rows from
available height capped at 8, tiles centered in their cells below the
36px taskbar).

NOTE: the montage labels grid slot i as APPS[i], which is only valid
for the DEFAULT icon layout.  icon_order[] persists across reboots, so
after the user rearranges icons the labels will no longer match the
tiles — re-capture from a fresh boot with default layout (or use the
'Reset to Default' button in Settings) for a faithful baseline.

Usage:
    python3 tools/icon-montage.py [source.ppm] [out.png]

Defaults:
    source = /tmp/desktop2.ppm
    out    = <this script's dir>/icon-baseline.png
"""

import os
import sys
from PIL import Image, ImageDraw

# ── Desktop constants (must match cpu/desktop.c) ─────────────────
ICON_GRID_COLS = 14
ICON_TILE_W = 64
ICON_TILE_H = 64
ICON_LABEL_H = 24
TASKBAR_H = 36
STATUSBAR_H = 28

APPS = [
    "Terminal", "Files", "Editor", "AppLib", "Trash", "Settings",
    "Music", "Calc", "Games", "Paint", "Monitor", "Clock", "Calendar",
    "Weather", "Organizer", "Security", "Gallery", "Browser",
    "QuickSettings",
]


def compute_grid(W, H):
    """Same layout as compute_icon_grid() in cpu/desktop.c."""
    cell_w = W // ICON_GRID_COLS
    if cell_w < ICON_TILE_W + 8:
        cell_w = ICON_TILE_W + 8  # same floor as compute_icon_grid()
    avail_h = H - TASKBAR_H - STATUSBAR_H
    min_cell_h = ICON_TILE_H + ICON_LABEL_H + 4
    rows = min(avail_h // min_cell_h, 8)
    if rows < 1:
        rows = 1
    cell_h = avail_h // rows
    return cell_w, cell_h, rows


def tile_rect(i, cell_w, cell_h, rows):
    col = i % ICON_GRID_COLS
    row = i // ICON_GRID_COLS
    px = col * cell_w + (cell_w - ICON_TILE_W) // 2
    py = TASKBAR_H + row * cell_h
    return px, py


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "/tmp/desktop2.ppm"
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(script_dir, "icon-baseline.png")

    if not os.path.exists(src):
        print(f"error: source file not found: {src}", file=sys.stderr)
        sys.exit(1)
    im = Image.open(src).convert("RGB")
    W, H = im.size
    cell_w, cell_h, rows = compute_grid(W, H)
    print(f"source: {src}  {W}x{H}  cell={cell_w}x{cell_h} rows={rows}")

    # Each cell: tile + the montage's own label text below it (we crop
    # ONLY the tile from the source — the desktop's own label strip is
    # intentionally excluded so it can't overlap the drawn name).
    cell_pad_x, cell_pad_y, label_h = 8, 8, 18
    cell_w_out = ICON_TILE_W + 2 * cell_pad_x
    cell_h_out = ICON_TILE_H + cell_pad_y + label_h + 8
    cols_out = 5
    rows_out = (len(APPS) + cols_out - 1) // cols_out
    pad = 12

    canvas = Image.new(
        "RGB",
        (cols_out * cell_w_out + (cols_out + 1) * pad,
         rows_out * cell_h_out + (rows_out + 1) * pad),
        (10, 8, 20),
    )
    draw = ImageDraw.Draw(canvas)

    for i, name in enumerate(APPS):
        px, py = tile_rect(i, cell_w, cell_h, rows)
        # Crop just the 64x64 tile (no label strip below it).
        crop = im.crop((px, py, px + ICON_TILE_W, py + ICON_TILE_H))
        col, row = i % cols_out, i // cols_out
        x0 = pad + col * (cell_w_out + pad)
        y0 = pad + row * (cell_h_out + pad)
        canvas.paste(crop, (x0 + cell_pad_x, y0 + cell_pad_y))
        # Tile outline for separation
        draw.rectangle(
            [x0 + cell_pad_x - 1, y0 + cell_pad_y - 1,
             x0 + cell_pad_x + ICON_TILE_W, y0 + cell_pad_y + ICON_TILE_H],
            outline=(60, 48, 96),
        )
        # Center the name under the tile (default PIL font ~11px tall).
        text_w = draw.textlength(name)
        draw.text(
            (x0 + cell_pad_x + (ICON_TILE_W - text_w) // 2,
             y0 + cell_pad_y + ICON_TILE_H + 4),
            name,
            fill=(230, 224, 248),
        )

    canvas.save(out)
    print(f"montage saved: {out}  {canvas.size[0]}x{canvas.size[1]}")


if __name__ == "__main__":
    main()
