#!/usr/bin/env python3
"""
transcode-videos.py — Convert non-MJPEG MP4s in host-files/ to MJPEG
so the Nyx OS video player can play them, and mux each video's audio
track in as MP3 so the kernel can decode it in the background (the
runtime MP3 pipeline in cpu/mp3async.c + cpu/mp3dec.c).

The kernel's video player (cpu/video.c) only decodes MJPEG-in-MP4.
Real-world videos (H.264/avc1, HEVC, ...) show "Codec not supported".
This tool runs at build time (wired into the Makefile before the embed
step) and re-encodes any unsupported MP4 into MJPEG using gstreamer,
sized to fit under the embedded filesystem's per-file cap
(MAX_FILESIZE = 8 MiB, see include/fs.h).

Audio is muxed in as MPEG-1 Layer III (lamemp3enc) @ 44.1 kHz stereo,
which the kernel's integer MP3 decoder accepts.  The runtime flow:
video.c parses the MP4 sample tables to pull the MP3 samples out of
mdat, hands the elementary stream to the background decoder task, and
the AC'97 driver plays the resulting PCM.  The old 6 kHz 8-bit `.pcm`
sidecar is still generated as a fallback for files whose audio mux
fails or runs on hardware without the AC'97 card.

For each input file:
  1. Probe the video track's stsd sample entry.
  2. If already MJPEG (jpeg/mjpa/mjpg) — leave the VIDEO alone; if the
     .originals backup carries audio, re-mux the existing video track
     with a fresh MP3 audio track (no re-encode).
  3. Otherwise try a ladder of (resolution, fps, quality) configs with
     gst-launch-1.0 until the output fits the size budget, keeping the
     source aspect ratio, and encode the audio track to MP3 in the
     same pass.
  4. Self-verify the output with the same box-parsing logic the kernel
     uses (stsd formats for jpeg + .mp3, tkhd dims, mdat JPEG count).
  5. Back the original up to host-files/.originals/ and replace it in
     place, so the embedded filesystem picks up the playable version
     under the same name.

Requires gst-launch-1.0 (gstreamer1.0-tools + gst-libav + gst-plugins-good
+ gst-plugins-ugly or gst-libav for lamemp3enc).  If it's missing, files
are skipped with a warning — the build still succeeds.

Usage:
    python3 tools/transcode-videos.py                 # all host-files/*.mp4
    python3 tools/transcode-videos.py file1.mp4 ...   # specific files
    python3 tools/transcode-videos.py --budget 900000 file1.mp4
"""

import os
import struct
import subprocess
import sys
import shutil
import tempfile

# ── Constants (must match include/fs.h / embed-files.py) ─────────
MAX_FILESIZE = 16*1024*1024      # per-file cap for the embedded FS (16 MiB)
DEFAULT_BUDGET = 15000000        # leave headroom under the 16 MiB cap
# NOTE: the LADDER now tries 10/12 fps configs and higher quality.
# The 8 MiB budget accommodates 704x704@12fps q75 (~7 MB) which
# gives the smoothest motion and highest per-frame quality.

# Audio sidecar sample rate — must match include/audio.h AUDIO_PCM_RATE.
PCM_RATE = 6000

# MP3 audio track settings — must match what cpu/mp3dec.c accepts
# (MPEG-1 Layer III only): 44.1 kHz stereo, ~96 kbps CBR keeps the
# track compact (≈120 KB/min) so video resolution isn't sacrificed.
MP3_RATE = 44100

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
HOST_DIR = os.path.join(PROJECT_DIR, 'host-files')
ORIG_DIR = os.path.join(HOST_DIR, '.originals')

# Ladder of (max_dim, fps, quality) — first config whose output fits wins.
# The aspect ratio of the source is preserved (width computed from height).
# Spatial resolution is prioritised over fps: a 384px source displayed
# in the player's ~420px video area is nearly 1:1, while 256px needed a
# 1.6x upscale that looked blocky no matter how good the filter was.
LADDER = [
    # ── Ultra tier (16 MiB budget) ────────────────────────────────
    (960, 12, 80),       # ~14 MB — ultra quality, smoothest motion
    (960, 12, 75),       # ~12 MB — ultra quality at 12 fps
    (960, 10, 80),       # ~12 MB — ultra quality, 10 fps
    (960, 10, 75),       # ~10 MB — excellent quality at 10 fps
    (896, 12, 80),       # ~12 MB — near-ultra at 12 fps
    (896, 12, 75),       # ~10 MB — near-ultra quality
    (896, 10, 75),       # ~9 MB — excellent at 10 fps
    (832, 12, 80),       # ~11 MB — high-res at 12 fps
    (832, 12, 75),       # ~9 MB — high-res quality
    (768, 12, 80),       # ~9 MB — HD+ at 12 fps
    (768, 12, 75),       # ~8 MB — HD+ quality
    (768, 10, 80),       # ~8 MB — HD+ at 10 fps
    # ── HD tier (8 MiB budget) ──────────────────────────────────
    (704, 12, 75),       # ~7.0 MB — best quality + smoothest motion
    (704, 12, 70),       # ~6.2 MB — excellent quality at 12 fps
    (704, 12, 65),       # ~5.5 MB — good quality at 12 fps
    (704, 10, 75),       # ~6.0 MB — high quality, 10 fps
    (704, 10, 70),       # ~5.2 MB — high quality at 10 fps
    (704, 12, 60),       # ~4.8 MB — 12 fps, moderate quality
    (704, 10, 65),       # ~4.0 MB — smooth, good quality
    (704, 8, 75),        # ~4.5 MB — HD at 8fps, highest quality
    (704, 8, 70),        # ~3.5 MB — HD at 8fps, high quality
    (704, 8, 65),        # ~2.8 MB — proven baseline
    # ── Mid tier ────────────────────────────────────────────────
    (640, 12, 70),       # ~4.5 MB — smooth + high quality
    (640, 12, 65),       # ~3.8 MB — smooth + good quality
    (640, 10, 70),       # ~3.8 MB — good resolution + quality
    (640, 10, 65),       # ~3.2 MB — reliable mid-range
    (640, 12, 60),       # ~3.0 MB — 12 fps at 640
    (640, 8, 70),        # ~2.8 MB — moderate res, high quality
    (640, 8, 65),        # ~2.3 MB — reliable fallback
    (576, 12, 65),       # ~3.2 MB — 12 fps, good quality
    (576, 12, 60),       # ~2.6 MB — 12 fps, moderate quality
    (576, 10, 65),       # ~2.5 MB — smooth + good detail
    (576, 8, 60),        # ~1.8 MB — proven config
    # ── Low tier (fits 2 MiB or less) ───────────────────────────
    (512, 10, 60),       # ~2.0 MB — smooth, good detail
    (512, 8, 65),        # ~1.5 MB — reliable fallback
    (420, 10, 55),       # ~1.4 MB — native 1:1 display + smooth
    (420, 12, 55),       # ~1.6 MB — 12 fps at native size
    (420, 6, 55),        # ~961 KB — old reliable
    (384, 8, 60),        # ~1.0 MB — safe fallback
    (320, 12, 60),       # ~900 KB — smooth at small size
    (288, 12, 55),
    (256, 12, 60),
    (224, 12, 48),
    (192, 12, 45),
    (160, 10, 40),
    (128, 8, 38),
    (96,  8, 35),
]


# ── MP4 probing (mirrors cpu/video.c vd_parse_*) ─────────────────
def rd32(p):
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]


def probe(path):
    """Return (stsd_format, width, height, frame_count) or (None, 0, 0, 0)."""
    with open(path, 'rb') as fh:
        f = fh.read()
    fmt = None
    w = h = frames = 0

    j = f.find(b'stsd')
    if j >= 0 and j + 20 <= len(f):
        fmt = f[j + 16:j + 20].decode('latin1')

    k = f.find(b'tkhd')
    if k >= 0:
        payload = k + 4
        ver = f[payload]
        woff = 88 if ver == 1 else 76
        if payload + woff + 8 <= len(f):
            w = rd32(f[payload + woff:payload + woff + 4]) >> 16
            h = rd32(f[payload + woff + 4:payload + woff + 8]) >> 16

    s = f.find(b'stsz')
    if s >= 0:
        payload = s + 4
        if payload + 12 <= len(f):
            frames = rd32(f[payload + 8:payload + 12])

    return fmt, w, h, frames


def has_audio_track(path):
    """True when the file contains a sound handler track (hdlr 'soun')."""
    try:
        with open(path, 'rb') as fh:
            return b'soun' in fh.read()
    except OSError:
        return False


def has_mp3_track(path):
    """True when the file's stsd lists a '.mp3' (or mp4a) audio entry."""
    with open(path, 'rb') as fh:
        f = fh.read()
    for m in (b'.mp3', b'mp4a'):
        if m in f:
            return True
    return False


def is_mjpeg(fmt):
    return fmt in ('jpeg', 'mjpa', 'mjpg')


def count_jpeg_frames(path):
    """Count FFD8..FFD9 streams in mdat (mirrors vd_scan_frames)."""
    with open(path, 'rb') as fh:
        f = fh.read()
    md = f.find(b'mdat')
    if md < 0:
        return 0
    msz = rd32(f[md:md + 4])
    region = f[md + 8:md + 8 + msz - 8]
    return region.count(b'\xff\xd8')


# ── Transcode ────────────────────────────────────────────────────
def has_gstreamer():
    return shutil.which('gst-launch-1.0') is not None


def extract_audio(src, dst):
    """Extract the audio track as 8-bit unsigned mono PCM.

    Returns True on success; False when the file has no audio track
    or gstreamer fails (missing plugins etc).
    """
    pipeline = [
        'gst-launch-1.0', '-q',
        'filesrc', f'location={src}',
        '!', 'qtdemux', 'name=d',
        'd.audio_0', '!', 'queue',
        '!', 'decodebin',
        '!', 'audioconvert',
        '!', 'audioresample',
        '!', f'audio/x-raw,format=U8,channels=1,rate={PCM_RATE}',
        '!', 'filesink', f'location={dst}',
    ]
    try:
        r = subprocess.run(pipeline, capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired):
        return False
    ok = (r.returncode == 0 and os.path.exists(dst)
          and os.path.getsize(dst) > 0)
    if not ok and os.path.exists(dst) and os.path.getsize(dst) == 0:
        # A failed pipeline can leave a 0-byte stub that would shadow a
        # good sidecar on later runs — remove it so extraction retries.
        os.unlink(dst)
    return ok


def pcm_sidecar_path(mp4_path):
    return os.path.splitext(mp4_path)[0] + '.pcm'


def maybe_extract_audio(src, path):
    """Extract <name>.pcm next to path if missing; prints the outcome."""
    pcm_path = pcm_sidecar_path(path)
    if os.path.exists(pcm_path) and os.path.getsize(pcm_path) > 0:
        print(f"     audio sidecar ok ({os.path.getsize(pcm_path):,}B)")
        return True
    if extract_audio(src, pcm_path):
        print(f"  ♪  audio → {os.path.basename(pcm_path)} "
              f"({os.path.getsize(pcm_path):,}B @ {PCM_RATE} Hz)")
        return True
    print("     no audio track (or gstreamer failed) — silent video")
    return False


# The two legs of every muxing pipeline (video → jpegenc, audio →
# lamemp3enc), joined at `qtmux name=mux`.  Kept as lists so both
# run_gst (single source, re-encode) and remux_with_mp3 (video copied,
# audio from the original) reuse the identical audio leg.
def _audio_leg(pad, rate=MP3_RATE):
    return [
        pad, '!', 'queue',
        '!', 'decodebin',
        '!', 'audioconvert',
        '!', 'audioresample',
        '!', f'audio/x-raw,rate={rate},channels=2',
        '!', 'lamemp3enc', 'bitrate=96',
        '!', 'queue', '!', 'mux.audio_0',
    ]


def run_gst(src, dst, width, height, fps, quality, with_audio):
    """gst-launch: decode video, scale, re-encode MJPEG, and mux the
    audio track (when present) as MP3 in the same pass."""
    pipeline = [
        'gst-launch-1.0', '-q',
        'filesrc', f'location={src}',
        '!', 'qtdemux', 'name=d',
        'd.video_0', '!', 'queue',
        '!', 'decodebin',
        '!', 'videoconvert',
        '!', 'videoscale',
        '!', 'videorate',
        '!', f'video/x-raw,width={width},height={height},framerate={fps}/1',
        '!', 'jpegenc', f'quality={quality}',
        '!', 'queue', '!', 'mux.video_0',
    ]
    if with_audio:
        pipeline += _audio_leg('d.audio_0')
    pipeline += [
        'qtmux', 'name=mux', 'faststart=true',
        '!', 'filesink', f'location={dst}',
    ]
    try:
        r = subprocess.run(pipeline, capture_output=True, text=True, timeout=240)
        return r.returncode == 0 and os.path.getsize(dst) > 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def remux_with_mp3(video_src, audio_src, dst):
    """Re-mux an already-MJPEG file: copy its video track verbatim
    (no re-encode) and add an MP3 audio track from the original."""
    pipeline = [
        'gst-launch-1.0', '-q',
        'filesrc', f'location={video_src}',
        '!', 'qtdemux', 'name=v',
        'v.video_0', '!', 'queue', '!', 'mux.video_0',
        'filesrc', f'location={audio_src}',
        '!', 'qtdemux', 'name=a',
    ] + _audio_leg('a.audio_0') + [
        'qtmux', 'name=mux', 'faststart=true',
        '!', 'filesink', f'location={dst}',
    ]
    try:
        r = subprocess.run(pipeline, capture_output=True, text=True, timeout=240)
        return r.returncode == 0 and os.path.getsize(dst) > 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def transcode_one(path, budget, force=False, size=None):
    name = os.path.basename(path)
    fmt, w, h, frames = probe(path)
    if fmt is None:
        print(f"  ⚠  '{name}' — not an MP4 (no stsd), skipping")
        return False
    orig = os.path.join(ORIG_DIR, name)
    if is_mjpeg(fmt):
        # Already playable.  The MJPEG transcode itself is silent, so if
        # the .originals backup carries audio, mux it back in as MP3
        # (video track copied verbatim — no quality loss).  Otherwise
        # fall back to the old .pcm sidecar.
        if has_mp3_track(path):
            print(f"  ✔  '{name}' is MJPEG + MP3 audio ({w}x{h}, {frames} frames) — ok")
        elif os.path.exists(orig) and has_audio_track(orig):
            with tempfile.NamedTemporaryFile(suffix='.mp4', dir='/tmp',
                                             delete=False) as tf:
                tmp = tf.name
            try:
                if not has_gstreamer():
                    print("     ✗  gst-launch-1.0 not found — keeping silent video")
                elif remux_with_mp3(path, orig, tmp):
                    fsize = os.path.getsize(tmp)
                    if fsize > MAX_FILESIZE:
                        print(f"     muxed {fsize:,}B over the {MAX_FILESIZE:,}B cap — "
                              f"keeping silent video + sidecar")
                        maybe_extract_audio(orig, path)
                        return False
                    nframes = count_jpeg_frames(tmp)
                    if nframes > 0 and has_mp3_track(tmp):
                        shutil.move(tmp, path)
                        print(f"  ♪  '{name}' + MP3 audio track "
                              f"({fsize:,}B, {nframes} frames) — video kept")
                        maybe_extract_audio(orig, path)  # sidecar fallback too
                        return True
                    os.unlink(tmp)
                    print("     MP3 re-mux produced no usable output — sidecar fallback")
                    maybe_extract_audio(orig, path)
                    return False
                else:
                    print("     MP3 re-mux failed — sidecar fallback")
                    maybe_extract_audio(orig, path)
                    return False
            finally:
                if os.path.exists(tmp):
                    os.unlink(tmp)
        else:
            print(f"  ✔  '{name}' is already MJPEG ({w}x{h}, {frames} frames) — ok")
            maybe_extract_audio(orig, path)
        if not force:
            return False
        print(f"  ▶  '{name}' forced re-transcode")
        src = orig if os.path.exists(orig) else path
    else:
        src = path
        print(f"  ▶  '{name}' codec '{fmt}' ({w}x{h}, {frames} frames) — transcoding to MJPEG + MP3")
    # The ladder scales from the SOURCE's dimensions — when --force
    # re-encodes from the .originals backup, the current file may be a
    # smaller MJPEG, so probe src (the actual input to gst-launch).
    _, sw, sh, _ = probe(src)
    if sw and sh:
        w, h = sw, sh
    if not has_gstreamer():
        print(f"     ✗  gst-launch-1.0 not found — skipped (install gstreamer1.0-tools, gst-libav, gst-plugins-good)")
        return False

    with_audio = has_audio_track(src)

    # Preserve aspect ratio: scale so the source's larger dimension hits
    # max_dim (never upscale).  Falls back to a square when dims unknown.
    # An explicit --size override bypasses the ladder entirely.
    for cfg in ([tuple(size)] if size else LADDER):
        if size:
            width, height, fps, quality = cfg
        else:
            max_dim, fps, quality = cfg
            if w and h:
                scale = min(1.0, max_dim / max(w, h))
                width = max(2, int(w * scale / 2) * 2)
                height = max(2, int(h * scale / 2) * 2)
                # Source smaller than this ladder step on both axes:
                # the size is capped by the source, so don't also cut
                # the frame rate — keep the standard 12 fps.
                if width < max_dim and height < max_dim:
                    fps = 12
            else:
                width = height = max(2, max_dim - max_dim % 2)

        with tempfile.NamedTemporaryFile(suffix='.mp4', dir='/tmp', delete=False) as tf:
            tmp = tf.name
        try:
            if not run_gst(src, tmp, width, height, fps, quality, with_audio):
                continue
            fsize = os.path.getsize(tmp)
            if fsize > budget:
                os.unlink(tmp)
                print(f"     {width}x{height}@{fps}fps q{quality} = {fsize:,}B — over budget, shrinking")
                continue
            # Self-verify with kernel-equivalent parsing
            nframes = count_jpeg_frames(tmp)
            if nframes <= 0:
                os.unlink(tmp)
                print(f"     {width}x{height}@{fps}fps q{quality} — no JPEG frames found, trying next")
                continue
            got_mp3 = has_mp3_track(tmp)
            # Audio sidecar from the SOURCE track (before we overwrite)
            maybe_extract_audio(src, path)
            # Success: back up the SOURCE and replace in place.  Only
            # copy when src is the file itself (a genuine original) —
            # when src is an existing .originals backup (--force), the
            # pristine original is already preserved there and must
            # not be clobbered by an intermediate MJPEG generation.
            os.makedirs(ORIG_DIR, exist_ok=True)
            bak = os.path.join(ORIG_DIR, name)
            backed_up = (src == path) or (not os.path.exists(bak))
            if backed_up:
                shutil.copy2(path, bak)
            shutil.move(tmp, path)
            print(f"  ✔  '{name}' → {width}x{height}@{fps}fps q{quality}, "
                  f"{fsize:,}B, {nframes} frames"
                  + (", MP3 audio" if got_mp3 else ", silent")
                  + (" (original backed up to .originals/)" if backed_up else ""))
            return True
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)

    print(f"  ✗  '{name}' — no config fits under {budget:,}B; leaving original")
    return False


def main():
    args = sys.argv[1:]
    budget = DEFAULT_BUDGET
    force = False
    size = None
    if '--force' in args:
        force = True
        args.remove('--force')
    if '--budget' in args:
        i = args.index('--budget')
        budget = int(args[i + 1])
        del args[i:i + 2]
    if '--size' in args:
        i = args.index('--size')
        size = tuple(int(x) for x in args[i + 1:i + 5])
        if len(size) != 4:
            print('  ✗  --size needs 4 values: WIDTH HEIGHT FPS QUALITY')
            return 1
        del args[i:i + 5]

    if not args:
        args = sorted(
            os.path.join(HOST_DIR, n) for n in os.listdir(HOST_DIR)
            if n.lower().endswith('.mp4')
        )
    if not args:
        print('  ✔  No .mp4 files in host-files/ — nothing to transcode')
        return 0

    print(f'  Transcoder: budget {budget:,}B, gst-launch-1.0 '
          f'{"found" if has_gstreamer() else "MISSING"}')
    changed = 0
    for path in args:
        if os.path.basename(path).startswith('.'):
            continue
        if transcode_one(path, budget, force, size):
            changed += 1
    print(f'  Done: {changed} file(s) updated')
    return 0


if __name__ == '__main__':
    sys.exit(main())
