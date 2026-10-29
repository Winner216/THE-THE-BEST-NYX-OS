#!/usr/bin/env python3
"""Probe: does HMP mouse_move move the PS/2 cursor with -display none?

Boots to the desktop, dumps mouse.px/py from kernel state (via a
temporary serial print in the taskbar tick is NOT needed — instead we
screendump and diff the cursor area).  Simpler: watch the grab
indicator + cursor save/restore state indirectly.

Cheapest reliable signal: the desktop taskbar shows a QEMU input-grab
indicator based on mouse_last_activity().  But position isn't logged.

So: temporary kernel probe — kernel ticks a line "MPOS <px> <py>" via
an existing per-second serial print if present, else we add one here?
No kernel edits: use screendump diff of the cursor sprite area instead.

Plan:
  1. boot to desktop
  2. screendump a
  3. mouse_move -200 -200   (shove cursor to top-left corner)
  4. screendump b           (cursor must move → pixels change)
  5. mouse_move 40 0
  6. screendump c           (small move → small pixel change near b)
  7. mouse_button 1 / 0     (click opens something? just watch pixels)
Prints the three diffs.  If a→b is ~0, HMP mouse_move is dead with
-display none and the E2E needs another injection path.
"""
import subprocess, time, os, sys

WORK = "/tmp/mouseprobe"
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

try:
    wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    wait_log("DESKTOP UP", 120)
    time.sleep(5)

    send("screendump " + WORK + "/a.ppm", 0.8)
    send("mouse_move -300 -300", 0.5)
    send("mouse_move -300 -300", 0.5)
    send("screendump " + WORK + "/b.ppm", 0.8)
    send("mouse_move 60 30", 0.5)
    send("screendump " + WORK + "/c.ppm", 0.8)
    send("mouse_button 1", 0.15)
    send("mouse_button 0", 0.5)
    send("screendump " + WORK + "/d.ppm", 0.8)
    send("quit", 0.4)
    try: qemu.wait(timeout=10)
    except Exception: pass
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass

def diff_ppm(a, b):
    da = open(a, "rb").read(); db = open(b, "rb").read()
    ra = da.split(b"\n", 3)[3]; rb = db.split(b"\n", 3)[3]
    if len(ra) != len(rb): return -1
    return sum(1 for p in range(0, len(ra), 3) if ra[p:p+3] != rb[p:p+3])

print("a->b (big move):  ", diff_ppm(WORK+"/a.ppm", WORK+"/b.ppm"))
print("b->c (small move):", diff_ppm(WORK+"/b.ppm", WORK+"/c.ppm"))
print("c->d (click):     ", diff_ppm(WORK+"/c.ppm", WORK+"/d.ppm"))
