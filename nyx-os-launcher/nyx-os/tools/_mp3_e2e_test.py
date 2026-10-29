#!/usr/bin/env python3
"""_mp3_e2e_test.py — boot the Nyx ISO in QEMU with AC'97 routed to a
PulseAudio null sink, drive the GUI to open jace.mp4 (which now carries
an in-container MP3 audio track), and verify the decoded soundtrack
flows through the AC'97 DMA ring to the host capture.

Sequence: login (1234) → F2 (Files) → Enter (home) → B (Local Disk,
shift) → navigate to the video row → Enter.  The video auto-plays its
audio; capture for ~25 s and analyze the spectrum.
"""
import subprocess, time, os, sys, signal

WORK = "/tmp/mp3e2e"
os.makedirs(WORK, exist_ok=True)
SINK = "nyx_mp3_sink"
# QEMU's -serial file: driver APPENDS — a stale log from a previous
# run would make the LOGIN READY poll pass instantly while the guest
# is still booting.  Truncate before QEMU starts.
serial_path = os.path.join(WORK, "serial.log")
try:
    open(serial_path, "w").close()
except Exception:
    pass

def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)

# ── 1. PulseAudio null sink + monitor capture ─────────────────────
sh("pactl unload-module module-null-sink 2>/dev/null || true")
sh("pactl unload-module module-remap-sink 2>/dev/null || true")
out = sh(f"pactl load-module module-null-sink sink_name={SINK} sink_properties=device.description={SINK}")
print("null-sink:", out.stdout.strip())
# make it the default sink so QEMU's pa driver routes there
sh("pactl set-default-sink " + SINK)

# ── 2. Start the capture (monitor of the null sink) ───────────────
cap = subprocess.Popen(
    ["parec", "--device", f"{SINK}.monitor", "--format=s16le", "--rate=48000",
     "--channels=2", "--raw", f"{WORK}/cap.raw"],
    stdout=open(f"{WORK}/cap.raw", "wb"),
    stderr=subprocess.DEVNULL)
time.sleep(0.5)

# ── 3. QEMU with monitor stdin for key injection ──────────────────
qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-audiodev", "pa,id=snd", "-machine", "pcspk-audiodev=snd",
     "-device", "AC97,audiodev=snd",
     "-monitor", "stdio", "-serial", "file:" + WORK + "/serial.log",
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd="/home/winnerbek/Downloads/nyx-os-launcher/nyx-os")

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)

def keys(seq, gap=0.25):
    for k in seq.split():
        send("sendkey " + k, gap)

try:
    # Phase 1 — wait for the login screen (LOGIN READY marker on COM1).
    deadline = time.time() + 240
    while time.time() < deadline:
        if "LOGIN READY" in open(WORK + "/serial.log").read():
            break
        time.sleep(1)
    send("screendump " + WORK + "/s1_login.ppm", 0.5)
    keys("1 2 3 4 ret", 0.4)             # password 1234 + Enter
    # Phase 2 — wait for the desktop to be interactive (DESKTOP UP),
    # then a short settle so the icon pump / video pump quiet down
    # before we start injecting navigation keys (keys sent while the
    # guest is busy get dropped by the PS/2 controller buffer).
    deadline = time.time() + 120
    while time.time() < deadline:
        if "DESKTOP UP" in open(WORK + "/serial.log").read():
            break
        time.sleep(1)
    time.sleep(5)
    send("screendump " + WORK + "/s2_desktop.ppm", 0.5)
    send("sendkey f2", 1.5)              # open Files (home view)
    send("screendump " + WORK + "/s3_files.ppm", 0.5)
    # This PC home: 'B' cycles tiles (B x5 = Local Disk), ENTER opens.
    for _ in range(5):
        send("sendkey shift-b", 0.25)    # 'B' → next tile
    send("screendump " + WORK + "/s4_localdisk.ppm", 0.5)
    send("sendkey ret", 0.8)             # open Local Disk
    time.sleep(1.0)
    # Local Disk file list: navigate to jace.mp4 (row 13 in the
    # embedded FS order).  QEMU's PS/2 buffer drops keys while the
    # guest is busy, so a blind 13-press loop is unreliable — instead
    # send Down one at a time and poll the serial log ("SEL <row>") to
    # confirm we've reached row 13 before pressing Enter.
    target = "SEL 13 jace.mp4"
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            log = open(WORK + "/serial.log").read()
        except FileNotFoundError:
            log = ""
        if target in log:
            break
        send("sendkey down", 0.35)
    # Draining: Down keys already in QEMU's PS/2 buffer keep advancing
    # the selection AFTER we see the target.  Stop sending and wait for
    # the buffer to drain, then confirm the LAST selection is still 13.
    time.sleep(2.0)
    try:
        sel_lines = [l for l in open(WORK + "/serial.log").read().splitlines()
                     if l.startswith("SEL ")]
        last_sel = sel_lines[-1] if sel_lines else ""
        print("last selection line:", last_sel)
        # If stragglers pushed past row 13, walk back with Up keys.
        want = 13
        if last_sel.startswith("SEL "):
            try:
                cur = int(last_sel.split()[1])
            except Exception:
                cur = want
            while cur > want:
                send("sendkey up", 0.35)
                cur -= 1
    except Exception as e:
        print("sel-drain warn:", e)
    send("screendump " + WORK + "/s5_selected.ppm", 0.5)
    send("sendkey ret", 0.8)             # open jace.mp4
    send("screendump " + WORK + "/s6_video.ppm", 0.5)
    print("video opened, capturing audio…")
    time.sleep(18)                       # ~15 s of soundtrack
    send("screendump " + WORK + "/video.ppm", 1.0)
    send("quit", 0.5)
    qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass
    cap.terminate()
    try: cap.wait(timeout=5)
    except Exception: cap.kill()
    sh("pactl unload-module module-null-sink 2>/dev/null || true")

# ── 4. Analyze the capture ────────────────────────────────────────
import array, math
try:
    data = open(WORK + "/cap.raw", "rb").read()
    print("captured bytes:", len(data))
    n = len(data) // 4
    samples = array.array("h")
    samples.frombytes(data[:n * 4])
    # average L/R into mono, look at the non-silent fraction + RMS
    total = 0.0; active = 0; peak = 0
    for i in range(0, n, 480):          # 10 ms steps
        s = (abs(samples[i*2]) + abs(samples[i*2+1])) / 2
        total += s; peak = max(peak, s)
        if s > 80: active += 1
    steps = n // 480
    rms_avg = total / max(1, steps)
    pct = 100.0 * active / max(1, steps)
    print("RMS(mono) avg: %.1f  peak: %.0f  active 10ms-steps: %d/%d (%.0f%%)" % (
        rms_avg, peak, active, max(1, steps), pct))
    # FFT on a loud segment to confirm broadband (music-like) content
    # rather than a single tone.  Uses numpy if available; otherwise
    # the RMS/active metrics above are already sufficient.
    try:
        import numpy as np
        W0 = 24000  # 0.5 s
        arr = np.frombuffer(data[:n * 4], dtype='<i2').astype(np.float64)
        mono = arr.reshape(-1, 2).mean(axis=1)
        rms_win = [float(np.sqrt(np.mean(mono[i:i+W0]**2)))
                   for i in range(0, max(1, n - W0), 4800)]
        bi = int(np.argmax(rms_win)) * 4800 if rms_win else 0
        seg = mono[bi:bi + W0] - mono[bi:bi + W0].mean()
        sp = np.abs(np.fft.rfft(seg))
        freqs = np.fft.rfftfreq(W0, 1 / 44100)
        strong = int(np.sum(sp[(freqs > 60) & (freqs < 8000)]
                            > np.median(sp) * 50))
        top_idx = np.argsort(sp)[-5:][::-1]
        print("strong FFT bins (>50x median):", strong)
        print("top freqs (Hz):", [round(float(freqs[i]), 1) for i in top_idx])
    except Exception as e:
        print("FFT check skipped:", e)
    # verdict: real audio needs a decent RMS floor AND a decent
    # fraction of active steps; silence is ~0 for both
    if rms_avg > 30 and pct > 5:
        print("PASS: real audio flowed to AC97 (RMS %.1f, %.0f%% active)"
              % (rms_avg, pct))
    else:
        print("FAIL: SILENT CAPTURE — audio did not flow (RMS %.1f, %.0f%% active)"
              % (rms_avg, pct))
except Exception as e:
    print("analyze err:", e)
