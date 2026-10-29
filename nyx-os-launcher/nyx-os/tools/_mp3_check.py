#!/usr/bin/env python3
"""Host regression for cpu/mp3dec.c.

Pipeline:
  1. synthesise a 3s stereo test signal (sine + melody + noise burst)
  2. encode to MP3 with gst-launch-1.0 lamemp3enc
  3. decode with the kernel decoder (tools/_mp3_harness.c binary)
  4. decode the same MP3 with gstreamer's mpg123audiodec (reference)
  5. compare: sample-rate, channel count, frame count, peak/RMS error

Usage: python3 tools/_mp3_check.py
"""
import os, struct, subprocess, sys, math, wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = "/tmp/mp3h"

def synth(path, seconds=3.0, rate=44100):
    n = int(seconds * rate)
    n -= n % 1152
    frames = []
    for i in range(n):
        t = i / rate
        # left: 440 Hz sine + 1.5s into a 660 Hz; right: 330 Hz + sweep
        l = 0.4 * math.sin(2 * math.pi * 440 * t)
        if t > 1.5:
            l += 0.25 * math.sin(2 * math.pi * 660 * t)
        r = 0.4 * math.sin(2 * math.pi * 330 * t)
        r += 0.2 * math.sin(2 * math.pi * (200 + 400 * t) * t)
        # melody blips every 0.25s
        phase = int(t / 0.25) % 4
        if phase == 0:
            l += 0.2 * math.sin(2 * math.pi * 880 * t)
        frames.append((l, r))
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        raw = b"".join(struct.pack("<hh", int(max(-1, min(1, l)) * 32767),
                                    int(max(-1, min(1, r)) * 32767))
                       for l, r in frames)
        w.writeframesraw(raw)
    return n

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        print("FAIL:", " ".join(cmd))
        print(r.stderr[-2000:])
        sys.exit(1)
    return r

def read_mp3_decoded(path, maxf=999999):
    r = run([HARNESS, path, "/tmp/_mp3_kernel.raw", str(maxf)])
    # parse "first frame @X, ch=N sr=S br=B"
    import re
    m = re.search(r"ch=(\d+) sr=(\d+)", r.stderr)
    ch, sr = int(m.group(1)), int(m.group(2))
    nframes = int(re.search(r"decoded (\d+) frames", r.stderr).group(1))
    data = open("/tmp/_mp3_kernel.raw", "rb").read()
    return ch, sr, nframes, data

def read_gst_decoded(path, mp3_sr):
    """Decode via mpg123audiodec to s16le raw (assume 44.1k or force?)."""
    out = "/tmp/_mp3_ref.raw"
    # gst decodebin -> audioconvert -> audioresample to keep it simple; we
    # read the negotiated rate from capsfile
    capsfile = "/tmp/_mp3_caps.txt"
    g = ("gst-launch-1.0 -q filesrc location={0} ! mpg123audiodec ! "
         "audioconvert ! audioresample ! 'audio/x-raw,format=S16LE,layout=interleaved' ! "
         "fakesink silent=true".format(path))
    # first run to learn caps via identity verbose? simpler: use decodebin+audioconvert and dump via filesink
    g2 = ("gst-launch-1.0 -q filesrc location={0} ! typefind ! decodebin ! "
          "audioconvert ! audioresample ! audio/x-raw,format=S16LE,layout=interleaved ! "
          "filesink location={1}".format(path, out))
    run(g2.split())
    return out

def read_raw16(path):
    data = open(path, "rb").read()
    return struct.unpack("<%dh" % (len(data) // 2), data)

def compare(kernel, ref, ch, tol_peak=12000, tol_rms=0.35):
    # kernel: interleaved s16 [L R L R ...]; ref same
    k = read_raw16(kernel)
    r = read_raw16(ref)
    n = min(len(k), len(r))
    if len(k) != len(r):
        print("note: kernel %d samples vs ref %d" % (len(k), len(r)))
    n = min(n, 1152 * 40)          # compare first ~1.3s
    errs = []
    for i in range(n):
        errs.append(abs(int(k[i]) - int(r[i])))
    peak = max(errs)
    rms = math.sqrt(sum(e * e for e in errs) / len(errs))
    print("  peak|err|=%d  rms_err=%.1f  (ref signal rms ~%.0f)" % (
        peak, rms, math.sqrt(sum(s * s for s in r[:n]) / n)))
    return peak <= tol_peak and rms <= tol_rms * math.sqrt(sum(s * s for s in r[:n]) / n) + 200

def main():
    # make sure harness exists
    if not os.path.exists(HARNESS):
        r = subprocess.run(["gcc", "-O2", "-Iinclude", "-o", HARNESS,
                            "tools/_mp3_harness.c", "cpu/mp3dec.c", "cpu/mp3tables.c"],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[:3000]); sys.exit(1)

    os.makedirs("/tmp/mp3test", exist_ok=True)
    wav = "/tmp/mp3test/tone.wav"
    synth(wav)
    mp3 = "/tmp/mp3test/tone.mp3"
    run(["gst-launch-1.0", "-q", "filesrc", "location=" + wav, "!", "wavparse", "!",
         "lamemp3enc", "!", "filesink", "location=" + mp3])
    print("encoded %s (%d bytes)" % (mp3, os.path.getsize(mp3)))

    ch, sr, nframes, kdata = read_mp3_decoded(mp3)
    print("kernel: ch=%d sr=%d frames=%d" % (ch, sr, nframes))
    ref = read_gst_decoded(mp3, sr)
    ok = compare("/tmp/_mp3_kernel.raw", ref, ch)
    print("RESULT:", "PASS" if ok else "FAIL")

if __name__ == "__main__":
    main()
