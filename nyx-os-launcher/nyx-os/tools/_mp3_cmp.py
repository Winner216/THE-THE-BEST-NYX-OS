#!/usr/bin/env python3
"""Compare kernel-decoded MP3 PCM vs gstreamer reference for both videos."""
import numpy as np
import subprocess, os

FRAME = 1152

def deplanar(raw):
    total = len(raw) // 2            # int16 count
    nf = total // (2 * FRAME)
    k = raw[:nf * 2 * FRAME].reshape(nf, 2 * FRAME)
    inter = np.empty(nf * FRAME * 2, dtype=np.float64)
    inter[0::2] = k[:, 0:FRAME].ravel()
    inter[1::2] = k[:, FRAME:2 * FRAME].ravel()
    return inter

for n in ['jace', '6']:
    src = '/tmp/%s_x.mp3' % n
    kern = '/tmp/%s_kern.raw' % n
    ref = '/tmp/%s_ref.raw' % n
    if not os.path.exists(src):
        print(n, 'skip (no stream)')
        continue
    subprocess.run(['gst-launch-1.0', '-q', 'filesrc', 'location=' + src,
                    '!', 'mpegaudioparse', '!', 'mpg123audiodec', '!',
                    'audioconvert', '!', 'audioresample', '!',
                    'audio/x-raw,format=S16LE,rate=44100,channels=2', '!',
                    'filesink', 'location=' + ref], check=False,
                   stderr=subprocess.DEVNULL)
    if not os.path.exists(ref):
        print(n, 'gst produced no ref')
        continue
    k = deplanar(np.fromfile(kern, dtype='<i2').astype(np.float64))
    r = np.fromfile(ref, dtype='<i2').astype(np.float64)
    print(n, 'kernel interleaved:', len(k), 'gst interleaved:', len(r))
    nn = min(len(k), len(r)) // 2 * 2
    k = k[:nn].reshape(-1, 2).mean(axis=1)
    r = r[:nn].reshape(-1, 2).mean(axis=1)
    print(n, 'mono:', len(k))
    # windowed correlation over the common prefix (skip long FFT)
    win = 120000
    best = 0.0; bestlag = 0
    for lag in range(0, 2001, 50):
        if lag + win > nn: break
        c = np.corrcoef(k[lag:lag + win], r[:win])[0, 1]
        if c > best:
            best = c; bestlag = lag
    print('%s: window=%d best_corr=%.4f lag=%d' % (n, min(win, nn), best, bestlag))
    os.unlink(ref)
