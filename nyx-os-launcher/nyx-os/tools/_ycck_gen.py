#!/usr/bin/env python3
"""Minimal pure-Python JPEG encoder for validating the kernel's 4-component
(CMYK/YCCK) color conversion.

Usage: _ycck_gen.py <out.jpg> <mode>
  mode 'cmyk' -> plain CMYK, Adobe APP14 transform=0.  Stores C/M/Y/K with
                 the Adobe convention (0 = max ink, 255 = no ink), using
                 the standard naive CMYK split (K_ink = 255 - max(r,g,b)).
  mode 'ycck' -> GCR YCCK, Adobe APP14 transform=2.  The Y/Cb/Cr planes are
                 computed from the GCR-reduced inverted-CMY image (i.e. the
                 YCbCr of R'=r+K_ink etc.), and K is stored inverted.

With the kernel's formulas
    plain CMYK:  R = max(0, C_stored + K_stored - 255)
    YCCK:        R = max(0, R_ycc     + K_stored - 255)
both modes must reproduce the source RGB exactly.
"""
import struct, sys, math

# ---- Standard Annex K Huffman tables (luma+chroma share table 0) ----
DC_BITS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
DC_VALS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
AC_BITS = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d]
AC_VALS = [0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,
           0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,
           0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,
           0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,
           0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,
           0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,
           0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,
           0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
           0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
           0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,
           0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,
           0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa]


def build_huff(bits, vals):
    codes = {}
    code = 0
    k = 0
    for ln in range(1, 17):
        for _ in range(bits[ln - 1]):
            codes[vals[k]] = (ln, code)
            k += 1
            code += 1
        code <<= 1
    return codes


DC_HUFF = build_huff(DC_BITS, DC_VALS)
AC_HUFF = build_huff(AC_BITS, AC_VALS)


class BitWriter:
    def __init__(self):
        self.bits = []

    def put(self, n, val):
        for i in range(n - 1, -1, -1):
            self.bits.append((val >> i) & 1)


class Jenc:
    def __init__(self, w, h, quant):
        self.w, self.h = w, h
        self.quant = quant

    def write_marker(self, marker, payload=b""):
        out = bytearray([0xFF, marker])
        out += struct.pack('>H', len(payload) + 2)
        out += payload
        return bytes(out)

    def quant_table_seg(self, tid):
        payload = bytes([tid])
        for v in self.quant:
            payload += bytes([max(1, min(255, v))])
        return self.write_marker(0xDB, payload)

    def huff_seg(self, cls, tid, bits, vals):
        payload = bytes([(cls << 4) | tid])
        payload += bytes(bits)
        payload += bytes(vals)
        return self.write_marker(0xC4, payload)

    def header(self, comps, transform):
        sof = bytearray([8])
        sof += struct.pack('>HH', self.h, self.w)
        sof += bytes([len(comps)])
        for cid, h, v, tq in comps:
            sof += bytes([cid, (h << 4) | v, tq])
        out = b'\xff\xd8'
        app14 = b'Adobe' + struct.pack('>H', 100) + struct.pack('>HH', 0, 0) + bytes([transform])
        out += self.write_marker(0xEE, app14)
        out += self.write_marker(0xC0, bytes(sof))
        out += self.quant_table_seg(0)
        out += self.huff_seg(0, 0, DC_BITS, DC_VALS)
        out += self.huff_seg(1, 0, AC_BITS, AC_VALS)
        return out

    def dct2d(self, blk):
        out = [[0.0] * 8 for _ in range(8)]
        for u in range(8):
            for v in range(8):
                cu = 1.0 / math.sqrt(2) if u == 0 else 1.0
                cv = 1.0 / math.sqrt(2) if v == 0 else 1.0
                s = 0.0
                for y in range(8):
                    for x in range(8):
                        s += blk[y][x] * math.cos((2*x+1)*u*math.pi/16) * math.cos((2*y+1)*v*math.pi/16)
                out[v][u] = 0.25 * cu * cv * s
        return out

    def encode_block(self, bw, block, prev_dc):
        shifted = [[block[y][x] - 128.0 for x in range(8)] for y in range(8)]
        dct = self.dct2d(shifted)
        zig = []
        for s in range(15):
            if s % 2 == 0:
                for i in range(min(7, s), max(0, s-7)-1, -1):
                    zig.append(dct[i][s-i])
            else:
                for i in range(max(0, s-7), min(7, s)+1):
                    zig.append(dct[i][s-i])
        q = [zig[i] / self.quant[i] for i in range(64)]
        dc = round(q[0]) - prev_dc
        cat = 0
        av = abs(dc)
        while av > 0:
            cat += 1
            av >>= 1
        out = []
        out.append(DC_HUFF[cat])
        if cat:
            out.append((cat, dc + ((1 << cat) - 1) if dc < 0 else dc))
        run = 0
        k = 1
        while k < 64:
            v = int(round(q[k]))
            if v == 0:
                run += 1
                k += 1
                continue
            r = run
            while r > 15:
                out.append(AC_HUFF[0xF0])
                r -= 16
            cat2 = 0
            av2 = abs(v)
            while av2 > 0:
                cat2 += 1
                av2 >>= 1
            sym = (r << 4) | cat2
            out.append(AC_HUFF[sym])
            out.append((cat2, v + ((1 << cat2) - 1) if v < 0 else v))
            run = 0
            k += 1
        out.append(AC_HUFF[0x00])
        return out, round(q[0])

    def flush_bits(self, bw):
        bits = bw.bits
        while len(bits) % 8:
            bits.append(1)
        out = bytearray()
        for i in range(0, len(bits), 8):
            b = 0
            for j in range(8):
                b = (b << 1) | bits[i + j]
            out.append(b)
            if b == 0xFF:
                out.append(0)
        out.append(0xFF)
        out.append(0xD9)
        return bytes(out)

    def sos(self, comps, planes):
        nsc = len(comps)
        body = bytearray([nsc])
        for cid, h, v, tq, td, ta in comps:
            body += bytes([cid, (td << 4) | ta])
        body += bytes([0, 63, 0])
        out = self.write_marker(0xDA, bytes(body))
        bw = BitWriter()
        prev = [0] * len(comps)
        for c in range(len(comps)):
            blk = planes[c]
            dcbits, dcval = self.encode_block(bw, blk, prev[c])
            prev[c] = dcval
            for item in dcbits:
                n, v = item
                bw.put(n, v)
        out += self.flush_bits(bw)
        return out


def to_planes(rgb, mode):
    """rgb: 8x8 of (r,g,b) with 0..255 floats.  Returns 4 planes.
    mode 'cmyk': Adobe-stored C/M/Y/K (0 = max ink).
    mode 'ycck': GCR-reduced inverted CMY as YCbCr + stored K.
    """
    n = 8
    if mode == 'cmyk':
        C = [[0.0]*n for _ in range(n)]
        M = [[0.0]*n for _ in range(n)]
        Y = [[0.0]*n for _ in range(n)]
        K = [[0.0]*n for _ in range(n)]
        for y in range(n):
            for x in range(n):
                r, g, b = rgb[y][x]
                mx = max(r, g, b)
                k_ink = 255.0 - mx                 # black coverage
                C[y][x] = 255.0 - (mx - r)         # 0 = max cyan ink
                M[y][x] = 255.0 - (mx - g)
                Y[y][x] = 255.0 - (mx - b)
                K[y][x] = 255.0 - k_ink
        return C, M, Y, K
    # YCCK: GCR-reduced image R' = r + k_ink (0..255), then YCbCr of it.
    Yp = [[0.0]*n for _ in range(n)]
    Cb = [[0.0]*n for _ in range(n)]
    Cr = [[0.0]*n for _ in range(n)]
    K = [[0.0]*n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            r, g, b = rgb[y][x]
            mx = max(r, g, b)
            k_ink = 255.0 - mx
            rp, gp, bp = r + k_ink, g + k_ink, b + k_ink
            Yp[y][x] = 0.299*rp + 0.587*gp + 0.114*bp
            Cb[y][x] = -0.168736*rp - 0.331264*gp + 0.5*bp + 128
            Cr[y][x] = 0.5*rp - 0.418688*gp - 0.081312*bp + 128
            K[y][x] = 255.0 - k_ink              # 0 = max black
    return Yp, Cb, Cr, K


def main():
    outpath = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else 'ycck'
    rgb = [[(0,0,0)]*8 for _ in range(8)]
    colors = [(255,0,0),(0,255,0),(0,0,255),(255,255,255),(255,255,0),(0,255,255),(255,0,255),(0,0,0)]
    for x in range(8):
        r, g, b = colors[x]
        for y in range(8):
            f = 1.0 - 0.12 * y
            rgb[y][x] = (int(r*f), int(g*f), int(b*f))
    quant = [16]*64
    enc = Jenc(8, 8, quant)
    planes = to_planes(rgb, mode)
    transform = 0 if mode == 'cmyk' else 2
    comps = [(1,1,1,0,0,0),(2,1,1,0,0,0),(3,1,1,0,0,0),(4,1,1,0,0,0)]
    sof_comps = [(1,1,1,0),(2,1,1,0),(3,1,1,0),(4,1,1,0)]
    out = enc.header(sof_comps, transform)
    out += enc.sos(comps, planes)
    with open(outpath, 'wb') as f:
        f.write(out)
    with open(outpath + '.rgb', 'wb') as f:
        f.write(b'P6\n8 8\n255\n')
        for y in range(8):
            for x in range(8):
                f.write(bytes(rgb[y][x]))


if __name__ == '__main__':
    main()
