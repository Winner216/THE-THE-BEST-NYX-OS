#!/usr/bin/env python3
"""_chess_dbg_probe.py — catch the moment QMP→guest input stops.

Runs against a nyx.iso whose CHESS.EXE carries TEMP DBG serial telemetry
("DBG hb poll=N accx=X accy=Y press=P cur=x,y" every ~0.6 s).  Boot, launch
the game, then repeat: park (closed-loop), click e2, send +20 px deltas.
Print the last DBG lines after each action so the exact step where the
guest stops receiving deltas is visible.
"""
import subprocess, time, os, sys, socket, json

WORK = "/tmp/chessdbg"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()
QMP_SOCK = os.path.join(WORK, "qmp.sock")
ROOT = "/home/winnerbek/Downloads/nyx-os-launcher/nyx-os"

BX, BY, SQ = 480, 70, 120
PANEL = (1476, 60, 1864, 520)
PX, PY = 1560, 200

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
    dl = time.time() + timeout
    while time.time() < dl:
        if sub in readlog(): return True
        time.sleep(1)
    return False
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
    if not step("shift-b"): return False
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

def qmp_connect(path, timeout=15):
    dl = time.time() + timeout
    while time.time() < dl:
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

def relm(dx=0, dy=0, pace=0.002):
    if dx: qmp_send([{"type": "rel", "data": {"axis": "x", "value": (1 if dx > 0 else -1)}}])
    if dy: qmp_send([{"type": "rel", "data": {"axis": "y", "value": (1 if dy > 0 else -1)}}])

def mouse_to(tx, ty, cur=[960, 540], settle=3.0):
    GAIN, pace = 3, 0.002
    dx, dy = tx - cur[0], ty - cur[1]
    for _ in range(abs(dx) * GAIN):
        relm(dx=1 if dx > 0 else -1); time.sleep(pace)
    for _ in range(abs(dy) * GAIN):
        relm(dy=1 if dy > 0 else -1); time.sleep(pace)
    cur[0], cur[1] = tx, ty
    time.sleep(settle)

def load_ppm(p):
    d = open(p, "rb").read()
    i = 2; vals = []
    while len(vals) < 3:
        while i < len(d) and d[i:i+1].isspace(): i += 1
        j = i
        while j < len(d) and not d[j:j+1].isspace(): j += 1
        vals.append(int(d[i:j])); i = j
    i += 1
    return vals[0], vals[1], d[i:]

def observe():
    p = WORK + "/obs.ppm"
    send("screendump " + p, 1.0)
    w, h, rgb = load_ppm(p)
    n = sx = sy = 0
    for y in range(PANEL[1], PANEL[3]):
        for x in range(PANEL[0], PANEL[2]):
            o = (y*w + x)*3
            if rgb[o] > 245 and rgb[o+1] > 245 and rgb[o+2] > 245:
                sx += x; sy += y; n += 1
    return (sx/n, sy/n) if n > 20 else None

def sq_screen(f, r):
    return (BX + f*SQ + SQ//2, BY + r*SQ + SQ//2)

def sel_count(path, f, r):
    w, h, rgb = load_ppm(path)
    x0, y0 = BX + f*SQ, BY + r*SQ
    n = 0
    for y in range(y0, y0+SQ):
        for x in range(x0, x0+SQ):
            o = (y*w + x)*3
            if (abs(rgb[o]-79) <= 25 and abs(rgb[o+1]-195) <= 25
                    and abs(rgb[o+2]-247) <= 25): n += 1
    return n

def dbg_tail(nlines=3, wait=2.0):
    time.sleep(wait)
    lines = [l for l in readlog().splitlines() if l.startswith("DBG ")]
    return lines[-nlines:]

cur = [960, 540]
try:
    assert wait_log("LOGIN READY", 90), "login"
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    assert wait_log("DESKTOP UP", 120), "desktop"
    time.sleep(5)
    send("sendkey f2", 1.5); time.sleep(1)
    for _ in range(5): step("shift-b")
    send("sendkey ret", 1.0); time.sleep(1.5)
    assert nav_to_name("CHESS.EXE"), "nav"
    send("sendkey ret", 1.5)
    assert wait_log("PE: loading CHESS.EXE", 60), "pe load"
    assert wait_log("[CHESS] up", 60), "chess up"
    time.sleep(3)

    print("=== baseline DBG (no input) ===")
    for l in dbg_tail(2, 3.0): print("  ", l)

    for trial in range(5):
        print(f"=== trial {trial} ===")
        # closed-loop park
        mouse_to(PX, PY, cur, settle=2.5)
        obs = observe()
        print("  park:", "None" if obs is None else f"({obs[0]:.0f},{obs[1]:.0f})")
        if obs is not None:
            cur[0], cur[1] = int(round(obs[0])), int(round(obs[1]))
            mouse_to(PX, PY, cur, settle=2.5)
            obs = observe()
            cur[0], cur[1] = int(round(obs[0])), int(round(obs[1])) if obs else cur[1]
        for l in dbg_tail(): print("  ", l)

        # click e2
        ex, ey = sq_screen(4, 6)
        mouse_to(ex, ey, cur, settle=3.0)
        qmp_send([{"type": "btn", "data": {"button": "left", "down": True}}])
        time.sleep(0.15)
        qmp_send([{"type": "btn", "data": {"button": "left", "down": False}}])
        time.sleep(0.5)
        send("screendump " + WORK + "/sel.ppm", 1.0)
        print("  e2 sel px:", sel_count(WORK + "/sel.ppm", 4, 6))
        for l in dbg_tail(): print("  ", l)

        # +20 px deltas x2
        cur[0], cur[1] = ex, ey
        for _ in range(60): relm(dx=1); time.sleep(0.002)
        cur[0] += 20
        time.sleep(3)
        for l in dbg_tail(): print("  ", l)

        # deselect (click empty d5)
        mouse_to(*sq_screen(3, 3), cur, settle=3.0)
        qmp_send([{"type": "btn", "data": {"button": "left", "down": True}}])
        time.sleep(0.15)
        qmp_send([{"type": "btn", "data": {"button": "left", "down": False}}])
        time.sleep(0.5)
        for l in dbg_tail(): print("  ", l)
    send("quit", 0.4); qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass
