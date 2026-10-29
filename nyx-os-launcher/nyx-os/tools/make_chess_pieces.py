#!/usr/bin/env python3
"""Generate user/chess/pieces.h from host-files/chess_pieces/*.svg.

The SVGs are simple polygon art: <path d="M x y L x y ... Z"/> with a
group fill + stroke.  This tool rasterizes them with a small software
rasterizer (per-subpixel point-in-polygon + even-odd distance-to-
segments stroke test, 3x3 supersampled for antialiasing) and emits C
sprite arrays in 0xAARRGGBB word format (straight alpha) that the CHESS
ring-3 task alpha-blends onto the board.

Map:   pawn/knight/bishop/rook/queen/king  x  white/black
Files: the user's names contain typos ("knioght", "queen blackc") so
       matching is token-based with graceful failure.
"""
import os, re, math, sys

PIECE_DIR = "host-files/chess_pieces"
OUT       = "user/chess/pieces.h"

SS        = 3          # supersample factor (3x3 = 9 samples / pixel)
OUT_SIZE  = 48         # sprite size in final pixels
VIEW      = 100.0      # SVG viewBox is 0 0 100 100
STROKE_W  = 4.0        # group stroke-width in the sources

KINDS = ["pawn", "knight", "bishop", "rook", "queen", "king"]

def hex3(s):
    s = s.lstrip('#')
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))

# ── Path parsing: d="M x y L x y ... Z" → list of closed subpaths ──
def parse_paths(text):
    polys = []
    for m in re.finditer(r'd="([^"]+)"', text):
        toks = re.findall(r'([MLZmlz])|(-?\d+(?:\.\d+)?)', m.group(1))
        subs, cur = [], []
        i = 0
        cmd = None
        while i < len(toks):
            c, num = toks[i]
            if c:
                cmd = c
                if cmd in 'Zz':
                    if len(cur) >= 3:
                        subs.append(cur)
                    cur = []
                i += 1
            else:
                # a coordinate pair belongs to the current command
                if cmd in 'MmLl' and i + 1 < len(toks):
                    x = float(toks[i][1]); y = float(toks[i + 1][1])
                    if cmd == 'M' or cmd == 'm':
                        if cur:            # implicit new subpath
                            if len(cur) >= 3: subs.append(cur)
                            cur = []
                    cur.append((x, y))
                    cmd = 'L'              # subsequent pairs = lineto
                    i += 2
                else:
                    i += 1
        if len(cur) >= 3:
            subs.append(cur)
        polys.extend(subs)
    return polys

def point_in_poly(px, py, poly):
    inside = False
    n = len(poly)
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > py) != (yj > py):
            xint = (xj - xi) * (py - yi) / (yj - yi) + xi
            if px < xint:
                inside = not inside
        j = i
    return inside

def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    if l2 <= 0.0:
        return math.hypot(px - ax, py - ay)
    t = ((px - ax) * dx + (py - ay) * dy) / l2
    if t < 0.0: t = 0.0
    if t > 1.0: t = 1.0
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))

def stroke_crossings(px, py, polys):
    """Even-odd test against the stroke centerline: a sample is 'on the
    stroke' when a horizontal ray crosses an odd number of stroke
    segments (segments within STROKE_W/2 vertically)."""
    crossings = 0
    half = STROKE_W / 2.0
    for poly in polys:
        n = len(poly)
        for i in range(n):
            ax, ay = poly[i]
            bx, by = poly[(i + 1) % n]
            if (ay > py) != (by > py):
                xint = (bx - ax) * (py - ay) / (by - ay) + ax
                if abs(py - (ay if by > ay else by)) <= half or True:
                    pass
                d = seg_dist(px, py, ax, ay, bx, by)
                if d <= half:
                    crossings += 1
    return crossings & 1

def rasterize(polys):
    """Return OUT_SIZE*OUT_SIZE 0xAARRGGBB words."""
    assert len(polys) >= 1
    fill, stroke = polys_fill_color, polys_stroke_color
    half = STROKE_W / 2.0
    step = 1.0 / SS
    out = []
    for j in range(OUT_SIZE):
        for i in range(OUT_SIZE):
            fill_hits = stroke_hits = 0
            for b in range(SS):
                for a in range(SS):
                    sx = (i + (a + 0.5) * step) * VIEW / OUT_SIZE
                    sy = (j + (b + 0.5) * step) * VIEW / OUT_SIZE
                    inside = any(point_in_poly(sx, sy, p) for p in polys)
                    onstroke = False
                    if not inside:
                        # cheap reject: within half+eps of bbox?
                        for p in polys:
                            xs = [pt[0] for pt in p]; ys = [pt[1] for pt in p]
                            if sx < min(xs) - half or sx > max(xs) + half \
                               or sy < min(ys) - half or sy > max(ys) + half:
                                continue
                            for k in range(len(p)):
                                ax, ay = p[k]
                                bx, by = p[(k + 1) % len(p)]
                                if seg_dist(sx, sy, ax, ay, bx, by) <= half:
                                    onstroke = True
                                    break
                            if onstroke:
                                break
                    if inside:
                        fill_hits += 1
                    if onstroke:
                        stroke_hits += 1
            n = SS * SS
            if stroke_hits >= fill_hits and stroke_hits:
                r, g, bch = stroke
                alpha = stroke_hits
            elif fill_hits:
                r, g, bch = fill
                alpha = fill_hits
            else:
                out.append(0)
                continue
            a8 = alpha * 255 // n
            if a8 < 8:
                out.append(0)
            else:
                out.append((a8 << 24) | (r << 16) | (g << 8) | bch)
    return out

def match_file(fname):
    n = fname.lower()
    kind = None
    for k in KINDS:
        if k in n:
            kind = k
            break
    if "knioght" in n:
        kind = "knight"
    if kind is None:
        return None
    color = 0 if "white" in n else (1 if "black" in n else None)
    if color is None:
        return None
    return kind, color

def main():
    files = sorted(os.listdir(PIECE_DIR))
    sprites = {}
    for f in files:
        if not f.lower().endswith(".svg"):
            continue
        m = match_file(f)
        if not m:
            print("  ?? skip (name):", f)
            continue
        kind, color = m
        text = open(os.path.join(PIECE_DIR, f), encoding="utf-8").read()
        gm = re.search(r'fill="(#[0-9a-fA-F]{6})"', text)
        sm = re.search(r'stroke="(#[0-9a-fA-F]{6})"', text)
        global polys_fill_color, polys_stroke_color
        polys_fill_color   = hex3(gm.group(1)) if gm else (255, 255, 255)
        polys_stroke_color = hex3(sm.group(1)) if sm else (0, 0, 0)
        polys = parse_paths(text)
        if not polys:
            print("  ?? skip (no paths):", f)
            continue
        px = rasterize(polys)
        cov = sum(1 for v in px if v)
        sprites[(color, kind)] = px
        print("  %-22s -> %-6s %-5s  polys=%2d  ink=%3d%%"
              % (f, kind, "white" if color == 0 else "black",
                 len(polys), cov * 100 // len(px)))
    missing = [(c, k) for c in (0, 1) for k in KINDS if (c, k) not in sprites]
    if missing:
        print("MISSING:", missing)
        sys.exit(1)

    with open(OUT, "w", encoding="utf-8") as w:
        w.write("// Generated by tools/make_chess_pieces.py from "
                "host-files/chess_pieces/*.svg — do not edit.\n")
        w.write("#ifndef PIECES_H\n#define PIECES_H\n#include <stdint.h>\n\n")
        w.write("#define PIECE_SPRITE_W %d\n#define PIECE_SPRITE_H %d\n"
                "#define PIECE_SPRITE_PX (PIECE_SPRITE_W * PIECE_SPRITE_H)\n\n"
                % (OUT_SIZE, OUT_SIZE))
        names = {}
        for color in (0, 1):
            for ki, kind in enumerate(KINDS):
                px = sprites[(color, kind)]
                cname = ("w" if color == 0 else "b") + kind
                names[(color, ki)] = "piece_" + cname
                w.write("static const uint32_t %s[PIECE_SPRITE_PX] = {"
                        % ("piece_" + cname))
                for i in range(0, len(px), 8):
                    w.write("\n    " + ", ".join("0x%08X" % v
                                                 for v in px[i:i + 8]) + ",")
                w.write("\n};\n\n")
        w.write("// Index: color*6 + kind (0=pawn 1=knight 2=bishop\n"
                "// 3=rook 4=queen 5=king; color 0=white 1=black).\n")
        w.write("#define PIECE_IDX(color, kind) ((color) * 6 + (kind))\n")
        w.write("static const uint32_t* const piece_sprites[12] = {\n")
        for color in (0, 1):
            for ki in range(6):
                w.write("    %s,\n" % names[(color, ki)])
        w.write("};\n\n#endif // PIECES_H\n")
    print("wrote", OUT)

if __name__ == "__main__":
    main()
