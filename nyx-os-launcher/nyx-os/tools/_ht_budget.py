#!/usr/bin/env python3
"""Verify node-budget limits for Huffman tree builders: gstreamer's
tables from jace.mp4, the standard JPEG Annex K luminance AC table, and
what limit is safe."""
import struct

def build_nodes(bits, values):
    nodes = [[-1, -1, -1]]
    count = 1
    code = 0
    vi = 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            if vi >= len(values): break
            vi += 1
            node = 0
            for b in range(length - 1, -1, -1):
                bit = (code >> b) & 1
                if nodes[node][bit] < 0:
                    nodes.append([-1, -1, -1])
                    nodes[node][bit] = count
                    count += 1
                node = nodes[node][bit]
            nodes[node][2] = vi
            code += 1
        code <<= 1
    return count

# --- gstreamer AC tables from jace.mp4 ---
f = open('host-files/jace.mp4', 'rb').read()
i = 0
md = None
while i + 8 <= len(f):
    sz, typ = struct.unpack('>I4s', f[i:i + 8])
    if sz == 1 or sz < 8: break
    if sz == 0: sz = len(f) - i
    if typ == b'mdat':
        md = f[i + 8:i + 8 + sz - 8]; break
    i += sz
soi = md.find(b'\xff\xd8')
eoi = md.find(b'\xff\xd9', soi)
frame = md[soi:eoi + 2]
i = 2
worst = 0
while i + 4 <= len(frame):
    if frame[i] != 0xFF:
        i += 1; continue
    m = frame[i + 1]
    if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
        i += 2; continue
    if m == 0xD9: break
    seglen = struct.unpack('>H', frame[i + 2:i + 4])[0]
    seg = frame[i + 4:i + 4 + seglen - 2]
    if m == 0xC4:
        o = 0
        while o + 17 <= len(seg):
            info = seg[o]; o += 1
            bits = list(seg[o:o + 16]); o += 16
            ns = sum(bits)
            values = seg[o:o + ns]; o += ns
            n = build_nodes(bits, values)
            worst = max(worst, n)
            print('gstreamer DHT cls=%d tid=%d syms=%d nodes=%d' % (info >> 4, info & 15, ns, n))
    i += 2 + seglen

# --- standard Annex K luminance AC table ---
annex_k_ac = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125]
n_ak = build_nodes(annex_k_ac, list(range(sum(annex_k_ac))))
print('Annex K luma AC: syms=%d nodes=%d' % (sum(annex_k_ac), n_ak))

# worst case across realistic distributions: all-symbols-at-16
n_w = build_nodes([0]*15 + [162], list(range(162)))
print('pathological 162@len16 nodes=%d' % n_w)
print('worst realistic seen: %d — 512 limit sufficient: %s' % (max(worst, n_ak), max(worst, n_ak) <= 512))
