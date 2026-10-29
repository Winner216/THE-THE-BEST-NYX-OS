#!/usr/bin/env python3
"""Validate an MP4 the way cpu/video.c parses it.

Reads the file, walks top-level boxes, finds stsd (codec), tkhd (dims),
mdhd (timescale/duration), stsz (sample count), and counts JPEG SOI
frames in mdat.  Mirrors the kernel parser so we can confirm a
transcoded MJPEG file will actually play in the OS.

NOTE: f.find(b'xxxx') returns the TYPE-field position; the box payload
starts at type_pos + 4 (after the 4-byte size field).  The kernel's
vd_find_box returns payload-relative offsets, i.e. type_pos + 4 too.
"""
import struct, sys

def rd32(p):
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]

def main():
    path = sys.argv[1]
    f = open(path, 'rb').read()
    print('size', len(f))

    # top-level boxes
    i = 0
    boxes = []
    while i + 8 <= len(f):
        sz, typ = struct.unpack('>I4s', f[i:i+8])
        if sz == 1 or sz < 8:
            break
        if sz == 0:
            sz = len(f) - i
        boxes.append((typ.decode('latin1'), i, sz))
        i += sz
    print('top-level:', [b[0] for b in boxes])

    # stsd sample entry format (kernel: payload offset 12)
    j = f.find(b'stsd')
    if j < 0:
        print('NO STSD — not an MP4')
        return 1
    fmt = f[j+16:j+20].decode('latin1')   # type_pos+4 (payload) + 12
    print('stsd fmt:', fmt)

    # tkhd dims (kernel vd_parse_tkhd: width at payload+76/88, h at +4)
    k = f.find(b'tkhd')
    if k >= 0:
        payload = k + 4
        ver = f[payload]
        woff = 88 if ver == 1 else 76
        w = rd32(f[payload+woff:payload+woff+4]) >> 16
        h = rd32(f[payload+woff+4:payload+woff+8]) >> 16
        print('tkhd dims:', w, 'x', h)

    # mdhd (kernel vd_parse_mdhd: timescale at payload+12/20)
    m = f.find(b'mdhd')
    if m >= 0:
        payload = m + 4
        ver = f[payload]
        toff = 20 if ver == 1 else 12
        ts = rd32(f[payload+toff:payload+toff+4])
        dur = rd32(f[payload+toff+4:payload+toff+8])
        print('mdhd: timescale', ts, 'dur', dur, '->', dur*1000//ts if ts else 0, 'ms')

    # stsz sample count (kernel vd_parse_stsz: count at payload+8)
    s = f.find(b'stsz')
    if s >= 0:
        payload = s + 4
        cnt = rd32(f[payload+8:payload+12])
        print('stsz sample_count:', cnt)

    # count JPEG streams in mdat (mirror vd_scan_frames)
    md = f.find(b'mdat')
    if md >= 0:
        msz = rd32(f[md:md+4])
        region = f[md+8:md+8+msz-8]
        n = region.count(b'\xff\xd8')
        print('mdat JPEG SOI count:', n)

    playable = fmt in ('jpeg', 'mjpa', 'mjpg')
    print('RESULT:', 'PLAYABLE' if playable else 'UNSUPPORTED')
    return 0 if playable else 1

if __name__ == '__main__':
    sys.exit(main())
