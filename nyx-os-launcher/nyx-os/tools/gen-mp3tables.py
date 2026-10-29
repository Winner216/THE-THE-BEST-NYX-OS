#!/usr/bin/env python3
"""gen-mp3tables.py — generate cpu/mp3tables.c + include/mp3tables.h

All constant tables for the Nyx OS MPEG-1 Layer III decoder are produced
here so they can be checked, regenerated and eyeballed from one place:

  * The 34 ISO/IEC 11172-3 Huffman tables, parsed from tools/HUFFCODE
    (the canonical table listing with code-word lengths and bit patterns).
    Each table becomes a binary trie (the dist10 approach) so non-canonical
    code assignments decode exactly.
  * pow43 (x^(4/3)) and pow2^quarter fixed-point lookup tables for the
    dequantizer.
  * The synthesis filterbank window (in the reference decoder's integer
    scale), the 32-band DCT-II constants and the IMDCT twiddle/window
    tables.
  * The scale-factor-band width tables (long/short/mixed per sample-rate
    index) and the scalefactor compression tables.

The generated files are committed to the tree (builds don't need Python);
re-run with:  python3 tools/gen-mp3tables.py
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HUFFCODE = os.path.join(ROOT, "tools", "HUFFCODE")
OUT_C = os.path.join(ROOT, "cpu", "mp3tables.c")
OUT_H = os.path.join(ROOT, "include", "mp3tables.h")

# ──────────────────────────────────────────────────────────────────
# 1. Huffman tables from the ISO listing
# ──────────────────────────────────────────────────────────────────

def parse_huffcode(path):
    tabs = {}
    cur = None
    with open(path) as f:
        for line in f:
            line = line.split('#')[0].strip()
            if not line:
                continue
            if line.startswith('.table'):
                parts = line.split()
                n = int(parts[1])
                tabs[n] = {'linbits': int(parts[4]) if len(parts) > 4 else 0,
                           'ref': None, 'rows': []}
                cur = n
            elif line.startswith('.reference'):
                tabs[cur]['ref'] = int(line.split()[1])
            elif line.startswith('.'):
                continue
            else:
                vals = line.split()
                # big-value tables: x y hlen hcod ; count1: v hlen hcod
                # (hcod is a BINARY bit string, read MSB first)
                if len(vals) == 4:
                    tabs[cur]['rows'].append((int(vals[0]), int(vals[1]),
                                              int(vals[2]), int(vals[3], 2)))
                elif len(vals) == 3:
                    tabs[cur]['rows'].append((int(vals[0]), int(vals[1]),
                                              int(vals[2], 2)))
    return tabs


def resolve(tabs, n):
    seen = set()
    while tabs[n]['ref'] is not None and n not in seen:
        seen.add(n)
        n = tabs[n]['ref']
    return n


def build_tree(rows, quad):
    """rows: [(sym_or(x,y), len, code)] -> (c0, c1, sym) trie arrays.
    Returns (nnodes, c0, c1, sym).

    sym is stored as VALUE+1 with 0 meaning "internal node": the raw
    symbol (x<<4)|y can itself be 0xFF (the (15,15) pair), which would
    collide with a 0xFF sentinel and make the decoder walk past the
    leaf.  Storing +1 keeps the sentinel 0 unambiguous and costs one
    extra uint16_t per node."""
    c0, c1, sym = [0xFFFF], [0xFFFF], [0]
    for row in rows:
        if quad:
            v, ln, code = row
            s = v
        else:
            x, y, ln, code = row
            s = (x << 4) | y
        node = 0
        for k in range(ln):
            bit = (code >> (ln - 1 - k)) & 1
            nxt = c1[node] if bit else c0[node]
            if nxt == 0xFFFF:
                c0.append(0xFFFF); c1.append(0xFFFF); sym.append(0)
                nxt = len(c0) - 1
                if bit:
                    c1[node] = nxt
                else:
                    c0[node] = nxt
            node = nxt
        sym[node] = s + 1
    return len(c0), c0, c1, sym


def gen_huffman(tabs):
    out = []
    for n in range(34):
        t = tabs.get(n)
        if t is None:
            raise SystemExit("missing huffman table %d" % n)
        src = resolve(tabs, n)
        nnodes, c0, c1, sym = build_tree(tabs[src]['rows'], n >= 32)
        out.append({'num': n, 'linbits': t['linbits'], 'quad': n >= 32,
                    'nnodes': nnodes, 'c0': c0, 'c1': c1, 'sym': sym})
    return out


# ──────────────────────────────────────────────────────────────────
# 2. Dequantizer tables
# ──────────────────────────────────────────────────────────────────

def gen_pow43():
    """x^(4/3) in Q12 (int32), 145 entries; entry k covers |x| = k-16."""
    q = 12
    vals = []
    for k in range(145):
        x = k - 16
        vals.append(int(round((abs(x) ** (4.0 / 3.0)) * (1 << q))) * (1 if x >= 0 else -1))
    return vals


def gen_pow2q():
    """2^((k-512)/4) in Q16 (int32, clamped) for k in [0,1024)."""
    q = 16
    vals = []
    for k in range(1024):
        v = 2.0 ** ((k - 512) / 4.0) * (1 << q)
        vals.append(min(int(v), 0x7FFFFFFF))
    return vals


# ──────────────────────────────────────────────────────────────────
# 3. DSP constants (reference decoder values)
# ──────────────────────────────────────────────────────────────────

G_WIN = [
    -1,26,-31,208,218,401,-519,2063,2000,4788,-5517,7134,5959,35640,-39336,74992,
    -1,24,-35,202,222,347,-581,2080,1952,4425,-5879,7640,5288,33791,-41176,74856,
    -1,21,-38,196,225,294,-645,2087,1893,4063,-6237,8092,4561,31947,-43006,74630,
    -1,19,-41,190,227,244,-711,2085,1822,3705,-6589,8492,3776,30112,-44821,74313,
    -1,17,-45,183,228,197,-779,2075,1739,3351,-6935,8840,2935,28289,-46617,73908,
    -1,16,-49,176,228,153,-848,2057,1644,3004,-7271,9139,2037,26482,-48390,73415,
    -2,14,-53,169,227,111,-919,2032,1535,2663,-7597,9389,1082,24694,-50137,72835,
    -2,13,-58,161,224,72,-991,2001,1414,2330,-7910,9592,70,22929,-51853,72169,
    -2,11,-63,154,221,36,-1064,1962,1280,2006,-8209,9750,-998,21189,-53534,71420,
    -2,10,-68,147,215,2,-1137,1919,1131,1692,-8491,9863,-2122,19478,-55178,70590,
    -3,9,-73,139,208,-29,-1210,1870,970,1388,-8755,9935,-3300,17799,-56778,69679,
    -3,8,-79,132,200,-57,-1283,1817,794,1095,-8998,9966,-4533,16155,-58333,68692,
    -4,7,-85,125,189,-83,-1356,1759,605,814,-9219,9959,-5818,14548,-59838,67629,
    -4,7,-91,117,177,-106,-1428,1698,402,545,-9416,9916,-7154,12980,-61289,66494,
    -5,6,-97,111,163,-127,-1498,1634,185,288,-9585,9838,-8540,11455,-62684,65290,
]

G_WIN_ROW = [29, 213, 459, 2037, 5153, 6574, 37489, 75038,
             104, 1567, 9727, 64019, -9975, -45, 146, -5]

G_SEC = [10.19000816, 0.50060302, 0.50241929, 3.40760851, 0.50547093, 0.52249861,
         2.05778098, 0.51544732, 0.56694406, 1.48416460, 0.53104258, 0.64682180,
         1.16943991, 0.55310392, 0.78815460, 0.97256821, 0.58293498, 1.06067765,
         0.83934963, 0.62250412, 1.72244716, 0.74453628, 0.67480832, 5.10114861]

G_AA = [[0.85749293, 0.88174200, 0.94962865, 0.98331459, 0.99551782, 0.99916056, 0.99989920, 0.99999316],
        [0.51449576, 0.47173197, 0.31337745, 0.18191320, 0.09457419, 0.04096558, 0.01419856, 0.00369997]]

G_MDCT_WIN = [
    [0.99904822, 0.99144486, 0.97629601, 0.95371695, 0.92387953, 0.88701083,
     0.84339145, 0.79335334, 0.73727734, 0.04361938, 0.13052619, 0.21643961,
     0.30070580, 0.38268343, 0.46174861, 0.53729961, 0.60876143, 0.67559021],
    [1, 1, 1, 1, 1, 1, 0.99144486, 0.92387953, 0.79335334, 0, 0, 0, 0, 0, 0,
     0.13052619, 0.38268343, 0.60876143],
]

G_TWID9 = [0.73727734, 0.79335334, 0.84339145, 0.88701083, 0.92387953, 0.95371695,
           0.97629601, 0.99144486, 0.99904822, 0.67559021, 0.60876143, 0.53729961,
           0.46174861, 0.38268343, 0.30070580, 0.21643961, 0.13052619, 0.04361938]

G_TWID3 = [0.79335334, 0.92387953, 0.99144486, 0.60876143, 0.38268343, 0.13052619]

G_PAN = [0, 1, 0.21132487, 0.78867513, 0.36602540, 0.63397460, 0.5, 0.5,
         0.63397460, 0.36602540, 0.78867513, 0.21132487, 1, 0]

G_PREAMP = [1, 1, 1, 1, 2, 2, 3, 3, 3, 2]

# MPEG1 scalefactor group sizes per scalefac_compress — the first four
# entries of each row of the ISO/reference g_scf_partitions table (the
# counts actually used by the decoder's 4-partition read loop).
G_SCF_PART = [
    [6, 5, 5, 5],
    [8, 9, 6, 12],
    [9, 9, 6, 12],
]

G_SCF_LONG = [
    # MPEG-1 rows 5,6,7 from the reference (44.1k, 48k, 32k).  Earlier
    # versions of this table accidentally carried MPEG-2 rows at indices
    # 0-1, which silently corrupted every MPEG-1 file that used short or
    # mixed blocks (the one[] band scalers came out wrong).
    [4, 4, 4, 4, 4, 4, 6, 6, 8, 8, 10, 12, 16, 20, 24, 28, 34, 42, 50, 54, 76, 158, 0],
    [4, 4, 4, 4, 4, 4, 6, 6, 6, 8, 10, 12, 16, 18, 22, 28, 34, 40, 46, 54, 54, 192, 0],
    [4, 4, 4, 4, 4, 4, 6, 6, 8, 10, 12, 16, 20, 24, 30, 38, 46, 56, 68, 84, 102, 26, 0],
]
G_SCF_SHORT = [
    [4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 6, 8, 8, 8, 10, 10, 10,
     12, 12, 12, 14, 14, 14, 18, 18, 18, 22, 22, 22, 30, 30, 30, 56, 56, 56, 0],
    [4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 6, 6, 6, 6, 10, 10, 10,
     12, 12, 12, 14, 14, 14, 16, 16, 16, 20, 20, 20, 26, 26, 26, 66, 66, 66, 0],
    [4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 6, 8, 8, 8, 12, 12, 12,
     16, 16, 16, 20, 20, 20, 26, 26, 26, 34, 34, 34, 42, 42, 42, 12, 12, 12, 0],
]
G_SCF_MIXED = [
    [4, 4, 4, 4, 4, 4, 6, 6, 4, 4, 4, 6, 6, 6, 8, 8, 8, 10, 10, 10,
     12, 12, 12, 14, 14, 14, 18, 18, 18, 22, 22, 22, 30, 30, 30, 56, 56, 56, 0],
    [4, 4, 4, 4, 4, 4, 6, 6, 4, 4, 4, 6, 6, 6, 6, 6, 6, 10, 10, 10,
     12, 12, 12, 14, 14, 14, 16, 16, 16, 20, 20, 20, 26, 26, 26, 66, 66, 66, 0],
    [4, 4, 4, 4, 4, 4, 6, 6, 4, 4, 4, 6, 6, 6, 8, 8, 8, 12, 12, 12,
     16, 16, 16, 20, 20, 20, 26, 26, 26, 34, 34, 34, 42, 42, 42, 12, 12, 12, 0],
]


def qint(v, q, bits=16):
    lo = -(1 << (bits - 1))
    hi = (1 << (bits - 1)) - 1
    return max(lo, min(hi, int(round(v * (1 << q)))))


def main():
    tabs_raw = parse_huffcode(HUFFCODE)
    tabs = gen_huffman(tabs_raw)
    pow43 = gen_pow43()
    pow2q = gen_pow2q()

    sec_q11 = [qint(v, 11) for v in G_SEC]
    aa_q15 = [qint(v, 15) for v in sum(G_AA, [])]
    mdct_q15 = [qint(v, 15) for v in sum(G_MDCT_WIN, [])]
    twid9_q15 = [qint(v, 15) for v in G_TWID9]
    twid3_q15 = [qint(v, 15) for v in G_TWID3]
    pan_q15 = [qint(v, 15) for v in G_PAN]

    with open(OUT_H, "w") as out:
        w = out.write
        w("// include/mp3tables.h — generated by tools/gen-mp3tables.py (do not edit)\n")
        w("//\n// Fixed-point constant tables for the Nyx OS MPEG-1 Layer III decoder.\n")
        w("#ifndef MP3TABLES_H\n#define MP3TABLES_H\n\n")
        w("#include <stdint.h>\n\n#define MP3_NTABLES 34\n\n")
        w("typedef struct {\n")
        w("    int16_t    linbits;   // extra linbits (big-value tables)\n")
        w("    uint8_t    quad;      // 1 = count1 quad table (32/33)\n")
        w("    uint16_t   nnodes;    // trie nodes\n")
        w("    const uint16_t* c0;   // child on bit 0 (0xFFFF = none)\n")
        w("    const uint16_t* c1;   // child on bit 1 (0xFFFF = none)\n")
        w("    const uint16_t* sym;  // symbol+1 at node (0 = internal; +1 so\n")
        w("                            // the (15,15)=0xFF leaf is unambiguous)\n")
        w("} Mp3HuffTable;\n\n")
        w("extern const Mp3HuffTable mp3_htabs[MP3_NTABLES];\n\n")
        w("// x^(4/3) in Q12 (int32); entry k covers |x| = k-16.\n")
        w("extern const int32_t mp3_pow43[145];\n")
        w("// 2^((k-512)/4) in Q16 (int32, clamped) — dequant quarter-exponents.\n")
        w("extern const int32_t mp3_pow2q[1024];\n\n")
        w("// Synthesis filterbank window (240 int32, reference scale).\n")
        w("extern const int32_t mp3_win[240];\n")
        w("// First-row window sums (16 int32).\n")
        w("extern const int32_t mp3_win_row[16];\n")
        w("// 32-band DCT-II constants (24 int16, Q11).\n")
        w("extern const int16_t mp3_sec[24];\n")
        w("// Antialias butterflies (16 int16, Q15).\n")
        w("extern const int16_t mp3_aa[16];\n")
        w("// IMDCT windows (36 int16, Q15).\n")
        w("extern const int16_t mp3_mdct_win[36];\n")
        w("// IMDCT twiddles (18 + 6 int16, Q15).\n")
        w("extern const int16_t mp3_twid9[18];\n")
        w("extern const int16_t mp3_twid3[6];\n")
        w("// Intensity-stereo pan factors (14 int16, Q15).\n")
        w("extern const int16_t mp3_pan[14];\n")
        w("// Preflag pretab (10 uint8).\n")
        w("extern const uint8_t mp3_pretab[10];\n")
        w("// Scale-factor band widths per sr_idx (3 x 23/40/40 uint8).\n")
        w("extern const uint8_t mp3_sfb_long[3][23];\n")
        w("extern const uint8_t mp3_sfb_short[3][40];\n")
        w("extern const uint8_t mp3_sfb_mixed[3][40];\n")
        w("// Scalefactor group counts: 0=long(6/5/5/6) 1=mixed 2=short.\n")
        w("extern const uint8_t mp3_scf_part[3][4];\n\n")
        w("#endif\n")

    with open(OUT_C, "w") as out:
        w = out.write
        w("// cpu/mp3tables.c — generated by tools/gen-mp3tables.py (do not edit)\n")
        w("#include <mp3tables.h>\n\n")

        # trie arrays per table
        for t in tabs:
            name = "t%02d" % t['num']
            w("static const uint16_t %s_c0[%d] = {\n" % (name, t['nnodes']))
            for i in range(0, t['nnodes'], 12):
                w("    " + ",".join(str(v) for v in t['c0'][i:i + 12]) + ",\n")
            w("};\n")
            w("static const uint16_t %s_c1[%d] = {\n" % (name, t['nnodes']))
            for i in range(0, t['nnodes'], 12):
                w("    " + ",".join(str(v) for v in t['c1'][i:i + 12]) + ",\n")
            w("};\n")
            w("static const uint16_t %s_sym[%d] = {\n" % (name, t['nnodes']))
            for i in range(0, t['nnodes'], 24):
                w("    " + ",".join(str(v) for v in t['sym'][i:i + 24]) + ",\n")
            w("};\n\n")

        w("const Mp3HuffTable mp3_htabs[MP3_NTABLES] = {\n")
        for t in tabs:
            name = "t%02d" % t['num']
            w("    { %d, %d, %d, %s_c0, %s_c1, %s_sym },\n" % (
                t['linbits'], 1 if t['quad'] else 0, t['nnodes'], name, name, name))
        w("};\n\n")

        def arr(name, vals, per, ctype="int32_t"):
            w("const %s %s[%d] = {\n" % (ctype, name, len(vals)))
            for i in range(0, len(vals), per):
                w("    " + ",".join(str(v) for v in vals[i:i + per]) + ",\n")
            w("};\n\n")

        arr("mp3_pow43", pow43, 8)
        arr("mp3_pow2q", pow2q, 8)
        arr("mp3_win", G_WIN, 8)
        arr("mp3_win_row", G_WIN_ROW, 8)
        arr("mp3_sec", sec_q11, 8, "int16_t")
        arr("mp3_aa", aa_q15, 8, "int16_t")
        arr("mp3_mdct_win", mdct_q15, 12, "int16_t")
        arr("mp3_twid9", twid9_q15, 9, "int16_t")
        arr("mp3_twid3", twid3_q15, 6, "int16_t")
        arr("mp3_pan", pan_q15, 7, "int16_t")
        arr("mp3_pretab", G_PREAMP, 10, "uint8_t")

        # 2D arrays: [sr_row][band] — the decoder indexes mp3_sfb_*[sr_idx]
        # as a row pointer, so every row must be the declared width (short/
        # mixed rows are zero-padded to 40 to match the header).
        def arr2d(name, rows, width, ctype="uint8_t"):
            w("const %s %s[%d][%d] = {\n" % (ctype, name, len(rows), width))
            for r in rows:
                padded = list(r) + [0] * (width - len(r))
                w("    {" + ",".join(str(v) for v in padded) + "},\n")
            w("};\n\n")

        arr2d("mp3_sfb_long", G_SCF_LONG, 23)
        arr2d("mp3_sfb_short", G_SCF_SHORT, 40)
        arr2d("mp3_sfb_mixed", G_SCF_MIXED, 40)
        arr2d("mp3_scf_part", G_SCF_PART, 4)

    print("wrote %s (%d bytes) and %s (%d bytes)" % (
        OUT_C, os.path.getsize(OUT_C), OUT_H, os.path.getsize(OUT_H)))


if __name__ == "__main__":
    main()
