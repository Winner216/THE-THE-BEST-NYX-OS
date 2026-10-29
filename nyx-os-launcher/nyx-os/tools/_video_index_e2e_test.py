#!/usr/bin/env python3
"""Verify the sample-table frame index fix in cpu/video.c end-to-end.

Boots the ISO with the AC'97 device (audio discarded via -audiodev
none — the DMA ring still runs, so the audio master clock advances in
real time and drives the video playhead), logs in, opens 6.mp4 then
jace.mp4 via the Files app (Local Disk rows follow the embedded FS
order: 6.mp4 = row 5, jace.mp4 = row 14), asserts the serial 'VDX idx='
equals the true stsz sample count (340 / 234 — previously the mdat
SOI scan reported 366/248 with bogus entries), and diffs two
screendumps taken ~4 s apart to confirm the picture advances.
"""
import subprocess, time, os, sys

WORK = "/tmp/vdxtest"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()
ROOT = "/home/winnerbek/Downloads/nyx-os-launcher/nyx-os"

def readlog():
    try:
        return open(serial_path, "rb").read().decode("latin1", "replace")
    except Exception:
        return ""

def wait_log(sub, timeout=240):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sub in readlog(): return True
        time.sleep(1)
    return False

def sel_lines():
    return [l for l in readlog().splitlines() if l.startswith("SEL ")]

def last_row():
    s = sel_lines()
    if not s: return None
    parts = s[-1].split()
    return int(parts[1]) if len(parts) >= 2 else None

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-audiodev", "none,id=snd", "-device", "AC97,audiodev=snd",
     "-monitor", "stdio", "-serial", "file:" + WORK + "/serial.log",
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

def step(key):
    """One navigation press, waiting until the guest processed it (the
    SEL marker count advanced) so no keys pile up in the PS/2 buffer."""
    before = len(sel_lines())
    send("sendkey " + key, 0.2)
    dl = time.time() + 4
    while time.time() < dl:
        if len(sel_lines()) > before: return True
        time.sleep(0.15)
    return False

def nav_row(target, label):
    cur = last_row()
    if cur is None: cur = 0   # initial selection (row 0) prints no marker
    guard = 0
    while cur != target and guard < 80:
        if not step("shift-b" if cur < target else "shift-a"):
            break
        nxt = last_row()
        if nxt is None or nxt == cur: break
        cur = nxt
        guard += 1
    print(label, "at row", cur, "(target", target, ")")
    return cur == target

def md5(p):
    import hashlib
    return hashlib.md5(open(p, "rb").read()).hexdigest()

def diff_ppm(a, b):
    def openit(p):
        d = open(p, "rb").read()
        i = 0
        while d[i:i+1] != b"\n": i += 1
        i += 1
        while d[i:i+1] != b"\n": i += 1
        i += 1
        return d, i
    da, oa = openit(a); db, ob = openit(b)
    if oa != ob: return -1
    diff = 0
    for p in range(oa, len(da), 3):
        if da[p:p+3] != db[p:p+3]: diff += 1
    return diff

results = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    results["desktop"] = wait_log("DESKTOP UP", 120)
    time.sleep(5)
    send("sendkey f2", 1.2)                            # Files (home)
    time.sleep(0.6)
    for _ in range(5): send("sendkey shift-b", 0.25)   # -> Local Disk tile
    send("sendkey ret", 0.8)                           # open Local Disk
    time.sleep(1.5)
    print("first sel:", sel_lines()[-1] if sel_lines() else None)
    # rows are 0-based: 6.mp4 row 5, jace.mp4 row 14
    results["nav_six"] = nav_row(5, "6.mp4")
    time.sleep(1.0)
    send("sendkey ret", 0.9)
    results["six_idx"] = wait_log("VDX idx=340", 90)
    send("screendump " + WORK + "/six1.ppm", 0.8)
    time.sleep(4)
    send("screendump " + WORK + "/six2.ppm", 0.8)
    send("sendkey esc", 1.2)                           # back to Local Disk
    time.sleep(1.0)
    if os.environ.get("VIDX_ONLY"):
        time.sleep(6)   # let the player run ~6s longer
    else:
        results["nav_jace"] = nav_row(14, "jace.mp4")
        time.sleep(1.0)
        send("sendkey ret", 0.9)
        results["jace_idx"] = wait_log("VDX idx=234", 90)
        send("screendump " + WORK + "/jace1.ppm", 0.8)
        time.sleep(4)
        send("screendump " + WORK + "/jace2.ppm", 0.8)
    send("quit", 0.4)
    qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass

log = readlog()
for l in log.splitlines():
    if l.startswith("VDX "): print("trace:", l)
print("MP3A lines:", log.count("MP3A ")
      if log else 0)
for n in ("six1", "six2", "jace1", "jace2"):
    p = WORK + "/" + n + ".ppm"
    if os.path.exists(p):
        print(n, "md5:", md5(p), os.path.getsize(p))
try:
    d6 = diff_ppm(WORK + "/six1.ppm", WORK + "/six2.ppm")
    print("6.mp4 whole-screen changed pixels over 4s:", d6)
except Exception as e:
    print("6.mp4 ppm diff err:", e); d6 = -1
try:
    dj = diff_ppm(WORK + "/jace1.ppm", WORK + "/jace2.ppm")
    print("jace whole-screen changed pixels over 4s:", dj)
except Exception as e:
    print("jace ppm diff err:", e); dj = -1
results["six_motion"] = d6 > 5000
results["jace_motion"] = dj > 5000
print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
ok = all(results.values())
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
