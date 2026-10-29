#!/usr/bin/env python3
"""_chess_capture.py — refresh ../chess_dbg/canvas_full.png with a live
in-game screenshot: boot, play 1. e4, wait for the AI reply (so the new
orange move-trace outline is visible), screendump, convert to PNG.
"""
import subprocess, time, os, sys, socket, json

WORK = "/tmp/chesscap"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()
QMP_SOCK = os.path.join(WORK, "qmp.sock")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-audiodev", "none,id=snd", "-device", "AC97,audiodev=snd",
     "-monitor", "stdio", "-serial", "file:" + serial_path,
     "-display", "none",
     "-qmp", "unix:" + QMP_SOCK + ",server,nowait"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

def readlog():
    try: return open(serial_path, "rb").read().decode("latin1", "replace")
    except Exception: return ""

def wait_log(sub, timeout=240):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sub in readlog(): return True
        time.sleep(1)
    return False

def qmp_connect(path, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = None
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            f = s.makefile("rw")
            json.loads(f.readline())
            f.write('{"execute":"qmp_capabilities"}\n'); f.flush()
            json.loads(f.readline())
            return s, f
        except Exception:
            if s: s.close()
            time.sleep(0.5)
    raise RuntimeError("QMP connect failed")

qmp, qmpf = qmp_connect(QMP_SOCK)

def qmp_send(events):
    qmpf.write(json.dumps({"execute": "input-send-event",
                           "arguments": {"events": events}}) + "\n")
    qmpf.flush()
    try: json.loads(qmpf.readline())
    except Exception: pass

def sel_lines():
    return [l for l in readlog().splitlines() if l.startswith("SEL ")]

def step(key):
    before = len(sel_lines())
    send("sendkey " + key, 0.2)
    dl = time.time() + 4
    while time.time() < dl:
        if len(sel_lines()) > before: return True
        time.sleep(0.15)
    return False

def nav_to_name(target_name):
    guard = 0
    if not step("shift-b"):
        return False
    while guard < 90:
        s = sel_lines()
        if s:
            parts = s[-1].split(None, 2)
            seen = parts[2] if len(parts) >= 3 else ""
            if seen == target_name: return True
            if seen < target_name:
                if not step("shift-b"): return False
            else:
                if not step("shift-a"): return False
        guard += 1
    return False

BX, BY, SQ = 480, 70, 120
cur = [960, 540]

def mouse_to(tx, ty):
    GAIN, pace = 3, 0.002
    dx, dy = tx - cur[0], ty - cur[1]
    sx = 1 if dx > 0 else -1
    sy = 1 if dy > 0 else -1
    for _ in range(abs(dx) * GAIN):
        qmp_send([{"type": "rel", "data": {"axis": "x", "value": sx}}])
        time.sleep(pace)
    for _ in range(abs(dy) * GAIN):
        qmp_send([{"type": "rel", "data": {"axis": "y", "value": sy}}])
        time.sleep(pace)
    cur[0], cur[1] = tx, ty
    time.sleep(3.0)

def click(tx, ty):
    mouse_to(tx, ty)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.15)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.5)

def sq_screen(f, r):
    return (BX + f*SQ + SQ//2, BY + r*SQ + SQ//2)

try:
    print("booting...", flush=True)
    assert wait_log("LOGIN READY", 120)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    assert wait_log("DESKTOP UP", 120)
    time.sleep(5)
    send("sendkey f2", 1.5); time.sleep(1)
    for _ in range(5): step("shift-b")
    send("sendkey ret", 1.0); time.sleep(1.5)
    assert nav_to_name("CHESS.EXE")
    send("sendkey ret", 1.5)
    assert wait_log("PE: loading CHESS.EXE", 60)
    assert wait_log("[CHESS] up", 60)
    time.sleep(3)

    x2, y2 = sq_screen(4, 6)                    # e2
    x4, y4 = sq_screen(4, 4)                    # e4
    click(x2, y2)
    click(x4, y4)
    time.sleep(8)                               # AI spinner + search + reply
    send("screendump " + WORK + "/full.ppm", 1.0)
    time.sleep(1)
finally:
    send("quit", 0.4)
    try: qemu.wait(timeout=10)
    except Exception: pass
    try: qemu.kill()
    except Exception: pass

# PPM → PNG (downscale 2× so the preview stays light)
from PIL import Image
img = Image.open(WORK + "/full.ppm")
img = img.resize((img.width // 2, img.height // 2), Image.LANCZOS)
out = os.path.join(os.path.dirname(ROOT), "chess_dbg", "canvas_full.png")
img.save(out)
print("wrote", out, flush=True)
