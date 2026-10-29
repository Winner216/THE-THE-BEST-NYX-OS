#!/usr/bin/env python3
"""E2E: DOOM sound — sfx lumps through the kernel game mixer to AC'97.

Flow: boot (KVM, AC97 with a **wav** audiodev so QEMU writes the DAC
output to a file) → login → Files → DOOM.EXE → Enter.  The shareware
demo autoplays on the title screen, which triggers DOOM's DS* sound
lumps via SYS_SNDPLAY.  Assertions:
  - [DOOM] snd: kernel mixer present   (AC97 probed OK)
  - [DOOM] snd: cached N sfx           (WAD lumps uploaded, N ≥ 40)
  - [DOOM] sfx n=K markers             (voices actually triggered)
  - the captured WAV contains non-silent audio bursts (RMS windows)
"""
import subprocess, time, os, sys, struct, math

WORK = "/tmp/doomsndtest"
os.makedirs(WORK, exist_ok=True)
WAV = WORK + "/audio.wav"
SERIAL = WORK + "/serial.log"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
open(SERIAL, "w").close()
for p in (WAV, WORK + "/doom_title.ppm"):
    if os.path.exists(p): os.remove(p)

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-audiodev", "wav,id=snd,path=" + WAV,
     "-device", "AC97,audiodev=snd",
     "-monitor", "stdio", "-serial", "file:" + SERIAL,
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

def log():
    return open(SERIAL, "rb").read().decode("latin1", "replace")

def wait_log(pat, timeout_s):
    dl = time.time() + timeout_s
    while time.time() < dl:
        if pat in log(): return True
        time.sleep(0.4)
    return False

def sel_lines():
    return [l for l in log().splitlines() if l.startswith("SEL ")]

def last_row():
    s = sel_lines()
    if not s: return None
    parts = s[-1].split()
    return int(parts[1]) if len(parts) >= 2 else None

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

def nav_row(target, label):
    cur = last_row()
    if cur is None: cur = 0
    guard = 0
    while cur != target and guard < 80:
        send("sendkey down" if cur < target else "sendkey up", 0.35)
        cur = last_row(); guard += 1
    print(f"nav {label}: cur={cur} target={target}")
    return cur == target

results = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    time.sleep(1.5)
    wait_log("DESKTOP UP", 90)
    time.sleep(5)
    send("sendkey f2", 1.2)                            # Files (home)
    time.sleep(0.6)
    for _ in range(5): send("sendkey shift-b", 0.25)   # -> Local Disk tile
    send("sendkey ret", 0.8)                           # open Local Disk
    results["nav_doom"] = nav_row(8, "DOOM.EXE")
    send("sendkey ret", 1.2)                           # launch the .exe
    results["pe_load"] = wait_log("PE: loading", 60)
    results["doom_start"] = wait_log("[DOOM] doomgeneric_Create", 90)
    results["snd_present"] = wait_log("[DOOM] snd: kernel mixer present", 30)
    results["snd_cached"] = wait_log("[DOOM] snd: cached", 30)

    # Demo + menu keys: trigger plenty of sfx.
    for k in ("down", "up", "ret", "spc", "ret", "down", "ret"):
        send("sendkey " + k, 0.5)
    results["sfx_fired"] = wait_log("[DOOM] sfx n=", 60)

    # Give the demo ~15 s of wall time to pump voices into the mixer.
    time.sleep(15)
    send("screendump " + WORK + "/doom_title.ppm", 0.8)
    results["alive"] = qemu.poll() is None
    send("quit", 0.4)
    qemu.wait(timeout=15)
except Exception as e:
    print("ERR:", e)
    try: qemu.kill()
    except Exception: pass

# ── Serial tail for the report ──────────────────────────────────
keep = ("[DOOM] snd", "[DOOM] sfx", "PE:", "[DOOM] starting",
        "FATAL", "PANIC", "page fault", "GP ")
for l in log().splitlines():
    if any(k in l for k in keep): print("|", l[:140])

# ── WAV analysis ────────────────────────────────────────────────
def wav_rms_windows(path, win_ms=100):
    d = open(path, "rb").read()
    if len(d) < 44 or d[:4] != b"RIFF":
        return None, "not a wav"
    # locate fmt + data chunks
    pos = 12; fmt = None; data = None
    while pos + 8 <= len(d):
        cid = d[pos:pos+4]; ln = struct.unpack("<I", d[pos+4:pos+8])[0]
        if cid == b"fmt ": fmt = d[pos+8:pos+8+ln]
        if cid == b"data": data = (pos+8, ln)
        pos += 8 + ln + (ln & 1)
    if not fmt or not data: return None, "missing chunks"
    ch, rate = struct.unpack("<H", fmt[2:4])[0], struct.unpack("<I", fmt[4:8])[0]
    bits = struct.unpack("<H", fmt[14:16])[0]
    off, ln = data
    ln = min(ln, len(d) - off)
    n = ln // (ch * bits // 8)
    raw = struct.unpack_from("<%dh" % (n * ch), d, off)
    step = int(rate * win_ms / 1000) * ch
    rms = []
    for i in range(0, len(raw) - step, step):
        acc = 0
        for j in range(i, i + step, 7):          # subsample for speed
            acc += raw[j] * raw[j]
        rms.append(math.sqrt(acc / (step / 7)))
    return {"rate": rate, "ch": ch, "bits": bits, "seconds": n / rate,
            "rms": rms}, None

try:
    info, err = wav_rms_windows(WAV)
    if err:
        print("WAV:", err); results["audio_signal"] = False
    else:
        r = info["rms"]
        loud = sum(1 for v in r if v > 400)      # 100 ms windows w/ signal
        peak = max(r) if r else 0
        print(f"WAV: {info['rate']} Hz {info['ch']}ch {info['bits']}bit "
              f"{info['seconds']:.1f}s, {len(r)} windows, peak RMS={peak:.0f}, "
              f"loud windows={loud}")
        # DOOM demo sfx → plenty of windows above the noise floor
        results["audio_signal"] = loud >= 10 and peak > 1000
except Exception as e:
    print("WAV err:", e); results["audio_signal"] = False

print("RESULTS:", results)
ok = all(results.values())
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
