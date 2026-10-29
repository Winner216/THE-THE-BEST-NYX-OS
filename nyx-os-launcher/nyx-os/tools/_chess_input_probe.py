#!/usr/bin/env python3
"""_chess_input_probe.py — measure QMP input delivery flakiness for CHESS.EXE.

Repeated trials: park the cursor in the clean panel region, move it ±30 px,
 screendump, and measure the white-crosshair centroid shift.  Then click on
e2 and count CLR_SEL pixels (with exactly the same settle the E2E tests use)
to see whether button events sometimes never arrive.
"""
import subprocess, time, os, sys, socket, json

WORK = "/tmp/chessinput"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()
QMP_SOCK = os.path.join(WORK, "qmp.sock")
ROOT = "/home/winnerbek/Downloads/nyx-os-launcher/nyx-os"

BX, BY, SQ = 480, 70, 120
PANEL = (1476, 60, 1864, 520)      # clean region for cursor centroid
PX, PY = 1560, 200                 # parking spot inside the panel

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

def rel(dx=0, dy=0):
    ev = []
    if dx: ev.append({"type": "rel", "data": {"axis": "x", "value": (1 if dx > 0 else -1)}})
    if dy: ev.append({"type": "rel", "data": {"axis": "y", "value": (1 if dy > 0 else -1)}})
    if ev: qmp_send(ev)

def mouse_to(tx, ty, cur=[960, 540], pace=0.002, settle=3.0):
    GAIN = 3
    dx = tx - cur[0]; dy = ty - cur[1]
    sx = 1 if dx > 0 else -1
    sy = 1 if dy > 0 else -1
    for _ in range(abs(dx) * GAIN):
        rel(dx=sx); time.sleep(pace)
    for _ in range(abs(dy) * GAIN):
        rel(dy=sy); time.sleep(pace)
    cur[0], cur[1] = tx, ty
    time.sleep(settle)

def click(tx, ty, cur=[960, 540]):
    mouse_to(tx, ty, cur)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.15)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.5)

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

def panel_centroid(path):
    w, h, rgb = load_ppm(path)
    sx = sy = n = 0
    for y in range(PANEL[1], PANEL[3]):
        for x in range(PANEL[0], PANEL[2]):
            o = (y*w + x)*3
            if rgb[o] > 245 and rgb[o+1] > 245 and rgb[o+2] > 245:
                sx += x; sy += y; n += 1
    if n == 0: return 0, 0.0, 0.0
    return n, sx/n, sy/n

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

    # ── focus forensics: keyboard vs mouse after launch ─────────
    send("screendump " + WORK + "/f0.ppm", 1.0)
    n0, cx0, cy0 = panel_centroid(WORK + "/f0.ppm")
    print("f0 panel cursor px:", n0)

    # keyboard: q should open the chat box (green focus border)
    send("sendkey q", 0.6); time.sleep(0.8)
    send("screendump " + WORK + "/fk.ppm", 1.0)
    w, h, rgb = load_ppm(WORK + "/fk.ppm")
    g = 0
    for y in range(934, 974):
        for x in range(16, 452):
            o = (y*w+x)*3
            if abs(rgb[o]-85) <= 20 and abs(rgb[o+1]-204) <= 20 and abs(rgb[o+2]-136) <= 20: g += 1
    print("sendkey q → chat border green px:", g)

    # murse: rel move while in chat focus (cursor crosshair should move)
    for _ in range(150): rel(dx=1); time.sleep(0.002)
    time.sleep(3)
    send("screendump " + WORK + "/fm.ppm", 1.0)
    n_m, cx_m, cy_m = panel_centroid(WORK + "/fm.ppm")
    print("after rel 50x: panel cursor px:", n_m, "centroid:", cx_m, cy_m)

    send("sendkey ret", 0.5)          # leave chat
    for trial in range(4):
        mouse_to(PX, PY, cur)
        send("screendump " + WORK + f"/c{trial}a.ppm", 1.0)
        n, cx, cy = panel_centroid(WORK + f"/c{trial}a.ppm")
        print(f"trial {trial} park: n={n} centroid=({cx:.1f},{cy:.1f})")

        # move +30 px in x → expect centroid +30
        for _ in range(90): rel(dx=1); time.sleep(0.002)
        cur[0] += 30
        time.sleep(3)
        send("screendump " + WORK + f"/c{trial}x.ppm", 1.0)
        n2, cx2, cy2 = panel_centroid(WORK + f"/c{trial}x.ppm")
        print(f"trial {trial} +30x: n={n2} centroid=({cx2:.1f},{cy2:.1f}) dx_c={cx2-cx:.1f}")

        # click e2 and count selection
        click(*sq_screen(4, 6), cur)
        time.sleep(1)
        send("screendump " + WORK + f"/c{trial}s.ppm", 1.0)
        print(f"trial {trial} e2 sel px:", sel_count(WORK + f"/c{trial}s.ppm", 4, 6))
        # deselect by clicking an empty square
        click(*sq_screen(0, 4), cur)
    send("quit", 0.4); qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass
