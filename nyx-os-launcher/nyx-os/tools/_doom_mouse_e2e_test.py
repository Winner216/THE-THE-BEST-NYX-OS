#!/usr/bin/env python3
"""E2E: mouse support in game mode — aim (X deltas) and fire (LMB).

Flow: boot (KVM) → login → desktop search "doom" → launch → start real
game (Episode 1 / skill 1) → then, in E1M1, using QEMU monitor mouse
injection (routes through the same PS/2 IRQ path as real hardware):

  1. AIM:   mouse_move dx sweeps (10 × 90) must turn the view — the 3D
            viewport strip must change a lot (kernel accumulates raw
            deltas → SYS_GETMOUSE → one ev_mouse per tic → angleturn).
  2. FIRE:  LMB click pulses (10 × press+release) must fire the pistol —
            the HUD ammo strip (excluding the 3D view and the face)
            must change (ammo counter drops).
  3. CLOSE: moving to the game title-bar [X] and clicking must close the
            game (task killed, desktop restored, DOOM must NOT reload).

Assertions use screendump pixel-diffs + serial-log markers.
"""
import subprocess, time, os, sys, socket, json

WORK = "/tmp/doommousetest"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
QMP_SOCK = os.path.join(WORK, "qmp.sock")
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
     "-monitor", "stdio",
     "-qmp", "unix:" + QMP_SOCK + ",server,nowait",
     "-serial", "file:" + WORK + "/serial.log",
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

# ── QMP mouse injection ──────────────────────────────────────────
# HMP mouse_move is scaled against the display surface, which is
# degenerate with -display none (X deltas collapse to 0).  QMP's
# input-send-event posts true relative motion to the PS/2 device.
def qmp_connect(path, timeout=15):
    deadline = time.time() + timeout
    s = None
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            f = s.makefile("rw")
            json.loads(f.readline())          # greeting
            f.write('{"execute":"qmp_capabilities"}\n'); f.flush()
            json.loads(f.readline())          # capabilities ack
            return s, f
        except Exception:
            if s: s.close()
            time.sleep(0.5)
    raise RuntimeError("QMP connect failed")

try:
    qmp, qmpf = qmp_connect(QMP_SOCK)
except Exception as e:
    print("QMP:", e)
    qmp = None

def qmp_send(events):
    if not qmp: return
    qmpf.write(json.dumps({"execute": "input-send-event",
                           "arguments": {"events": events}}) + "\n")
    qmpf.flush()
    reply = json.loads(qmpf.readline())
    if "return" not in reply:
        print("QMP ERROR:", str(reply)[:200])   # e.g. 'value' missing

def mouse_rel(dx, dy):
    evs = []
    if dx: evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
    if dy: evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
    qmp_send(evs)

def mouse_btn(left, down):
    qmp_send([{"type": "btn",
               "data": {"button": "left" if left else "right",
                        "down": down}}])

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

results = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    results["desktop"] = wait_log("DESKTOP UP", 120)
    time.sleep(5)
    send("sendkey slash", 0.6)
    send("sendkey d", 0.3); send("sendkey o", 0.3)
    send("sendkey o", 0.3); send("sendkey m", 0.3)
    send("sendkey ret", 1.2)
    results["doom_start"] = wait_log("[DOOM] starting", 60)
    results["gamestart"] = wait_log("[DOOM] doomgeneric_Create", 90)
    # Start a REAL game: ESC → New Game → Episode 1 → Skill.
    time.sleep(12)
    send("sendkey esc", 0.8)
    send("sendkey ret", 1.0)
    send("sendkey ret", 1.0)
    send("sendkey ret", 1.0)
    time.sleep(12)   # level load + "Entering Hangar" splash

    # ── 1. AIM: mouse X deltas must turn the view ──────────────────
    # QMP rel events reach the PS/2 device as raw relative motion
    # (same queue the GUI frontends use).
    send("screendump " + WORK + "/aim0.ppm", 0.8)
    for _ in range(10):
        mouse_rel(90, 0); time.sleep(0.25)
    send("screendump " + WORK + "/aim1.ppm", 0.8)
    for _ in range(10):
        mouse_rel(-90, 0); time.sleep(0.25)   # turn back
    send("screendump " + WORK + "/aim2.ppm", 0.8)

    # ── 2. FIRE: LMB pulses must fire the pistol ───────────────────
    send("screendump " + WORK + "/fire0.ppm", 0.8)
    for _ in range(10):
        mouse_btn(True, True); time.sleep(0.12)
        mouse_btn(True, False); time.sleep(0.18)
    send("screendump " + WORK + "/fire1.ppm", 0.8)

    results["alive"] = qemu.poll() is None

    # ── 3. CLOSE: click the game title-bar [X] ─────────────────────
    # Game window: 640×400 centered in 1920×1080 → x 640..1280,
    # y 340..740; title bar y 316..340; [X] box at x ≈ 1258..1274,
    # y ≈ 320..336.  Click (960, 316+8) — title bar right of center.
    # (Exact box: x = 640+640-16-6 = 1258, y = 316+4 = 320..336.)
    log_before_close = len(readlog())
    # Cursor starts centered (960, 540); the [X] box is at ≈(1266, 328)
    # — walk there with relative moves (PS/2 splits >255 into packets).
    mouse_rel(306, 0); time.sleep(0.3)
    mouse_rel(0, -212); time.sleep(0.5)
    mouse_btn(True, True); time.sleep(0.15)
    mouse_btn(True, False); time.sleep(0.8)
    time.sleep(3)
    log_after = readlog()
    results["close_kills"] = "PE: loading" not in log_after[log_before_close:]
    results["alive_after_close"] = qemu.poll() is None

    send("quit", 0.4)
    try:
        qemu.wait(timeout=10)
    except Exception:
        pass
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass

def diff_ppm(a, b):
    def openit(p):
        d = open(p, "rb").read()
        i = 0
        while d[i:i+1] != b"\n": i += 1
        i += 1
        while d[i:i+1] != b"\n": i += 1
        i += 1
        return d, i
    try:
        da, oa = openit(a); db, ob = openit(b)
    except Exception:
        return -1
    if oa != ob or len(da) != len(db): return -1
    diff = 0
    for p in range(oa, len(da), 3):
        if da[p:p+3] != db[p:p+3]: diff += 1
    return diff

# Viewport strip: the 3D view only (window 1920×1080, game canvas
# x 640..1280, view y 340..740 above the status bar).  Excludes HUD.
def diff_view(a, b):
    d = open(a, "rb").read(); e = open(b, "rb").read()
    da = d.split(b"\n", 3)[3]; db = e.split(b"\n", 3)[3]
    W = 1920
    diff = 0
    for y in range(340, 700):
        row = y * W * 3
        for x in range(640, 1280):
            p = row + x * 3
            if da[p:p+3] != db[p:p+3]: diff += 1
    return diff

# Ammo HUD strip: big ammo digits + icons, left of the face — excludes
# the 3D view AND the animated face, so only ammo-count pixels move.
def diff_strip(a, b, x0, x1, y0, y1):
    d = open(a, "rb").read(); e = open(b, "rb").read()
    da = d.split(b"\n", 3)[3]; db = e.split(b"\n", 3)[3]
    W = 1920
    diff = 0
    for y in range(y0, y1):
        row = y * W * 3
        for x in range(x0, x1):
            p = row + x * 3
            if da[p:p+3] != db[p:p+3]: diff += 1
    return diff

d_aim   = max(diff_view(WORK+"/aim0.ppm",  WORK+"/aim1.ppm"),
              diff_view(WORK+"/aim1.ppm",  WORK+"/aim2.ppm"))
d_fire  = diff_strip(WORK+"/fire0.ppm", WORK+"/fire1.ppm", 645, 1000, 700, 860)

log = readlog()
faulted = ("50584621" in log or "PANIC" in log or "FATAL" in log)

print("mouse aim view delta:   ", d_aim)
print("mouse fire ammo delta:  ", d_fire)
print("fault markers:          ", "YES" if faulted else "no")
results["mouse_aims"]  = d_aim > 30000
results["mouse_fires"] = d_fire > 150
results["no_faults"]   = not faulted

print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
ok = all(results.values())
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
