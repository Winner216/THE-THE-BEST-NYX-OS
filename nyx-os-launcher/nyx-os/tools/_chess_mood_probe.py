#!/usr/bin/env python3
"""Probe: does a real blunder flip the chess PC's mood face?

Plays 1. e4 2. Nf3, then walks the f3-knight to e5 (attacked by the
c6-knight, i.e. a genuine blunder).  After every AI reply, screendump
and count the mood-colored pixels inside the chat face box (canvas
x 358..426, y 20..90) plus a board reconstruction.
"""
import subprocess, time, os, sys, socket, json

WORK = "/tmp/chessmood"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()
QMP_SOCK = os.path.join(WORK, "qmp.sock")
ROOT = "/home/winnerbek/Downloads/nyx-os-launcher/nyx-os"

BX, BY, SQ = 480, 70, 120
FACE = (358, 20, 426, 90)          # face box + mood word region

MOODS = {
    "calm":   (156, 139, 200),     # CLR_DIM
    "smug":   (255, 184, 77),      # CLR_WARN
    "cocky":  (242, 236, 255),     # CLR_TEXT
    "wary":   (85, 204, 136),      # CLR_OK
    "panic":  (255, 85, 102),      # CLR_BAD
}

def sq_screen(fi, ri):
    return (BX + fi*SQ + SQ//2, BY + ri*SQ + SQ//2)

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

def qmp_connect(path, timeout=15):
    import socket, json
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

def mouse_to(tx, ty, cur=[960, 540]):
    GAIN, pace = 3, 0.002
    dx = tx - cur[0]; dy = ty - cur[1]
    sx = 1 if dx > 0 else -1
    sy = 1 if dy > 0 else -1
    for _ in range(abs(dx) * GAIN):
        qmp_send([{"type": "rel", "data": {"axis": "x", "value": sx}}]); time.sleep(pace)
    for _ in range(abs(dy) * GAIN):
        qmp_send([{"type": "rel", "data": {"axis": "y", "value": sy}}]); time.sleep(pace)
    cur[0], cur[1] = tx, ty
    time.sleep(3.0)

def click(tx, ty, cur=[960, 540]):
    mouse_to(tx, ty, cur)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": True}}]); time.sleep(0.15)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": False}}]); time.sleep(0.45)

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

WHITE_B = (246, 246, 250); BLACK_B = (12, 12, 16)
def _solid(rgb, w, x, y, want, tol, reach=6):
    o = (y*w + x)*3
    if abs(rgb[o]-want[0]) > tol or abs(rgb[o+1]-want[1]) > tol \
       or abs(rgb[o+2]-want[2]) > tol: return False
    for dx, dy in ((-reach, 0), (reach, 0), (0, -reach), (0, reach)):
        p = ((y+dy)*w + (x+dx))*3
        if abs(rgb[p]-want[0]) > tol or abs(rgb[p+1]-want[1]) > tol \
           or abs(rgb[p+2]-want[2]) > tol: return False
    return True

def read_board(path):
    w, h, rgb = load_ppm(path)
    rows = []
    for r in range(8):
        row = ""
        for f in range(8):
            x0 = BX + f*SQ; y0 = BY + r*SQ
            cw = cb = 0
            for y in range(y0+10, y0+SQ-10, 4):
                for x in range(x0+10, x0+SQ-10, 4):
                    if _solid(rgb, w, x, y, WHITE_B, 10):   cw += 1
                    elif _solid(rgb, w, x, y, BLACK_B, 26): cb += 1
            row += "P" if cw > 12 else ("p" if cb > 12 else ".")
        rows.append(row)
    return rows

def face_counts(path):
    w, h, rgb = load_ppm(path)
    x0, y0, x1, y1 = FACE
    out = {}
    for name, (R, G, B) in MOODS.items():
        n = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                o = (y*w + x)*3
                if abs(rgb[o]-R) <= 20 and abs(rgb[o+1]-G) <= 20 \
                   and abs(rgb[o+2]-B) <= 20: n += 1
        out[name] = n
    return out

cur = [960, 540]
try:
    assert wait_log("LOGIN READY", 90), "login"
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    assert wait_log("DESKTOP UP", 120), "desktop"
    time.sleep(5)
    send("sendkey f2", 1.5); time.sleep(1)
    for _ in range(5): step("shift-b")          # home tiles → Local Disk
    send("sendkey ret", 1.0); time.sleep(1.5)   # open Local Disk
    assert nav_to_name("CHESS.EXE"), "nav"
    send("sendkey ret", 1.5)
    assert wait_log("PE: loading CHESS.EXE", 60), "pe load"
    assert wait_log("[CHESS] up", 60), "chess up"
    time.sleep(3)
    mouse_to(1560, 200, cur)                    # park cursor on panel
    send("screendump " + WORK + "/s0_boot.ppm", 1.0)
    print("boot face:", face_counts(WORK + "/s0_boot.ppm"))

    def move(f1, r1, f2, r2):
        click(*sq_screen(f1, r1), cur)
        click(*sq_screen(f2, r2), cur)
        time.sleep(6)


    move(4, 6, 4, 4)                            # 1. e4
    send("screendump " + WORK + "/s1.ppm", 1.0)
    print("after e4 face:", face_counts(WORK + "/s1.ppm"))
    b1 = read_board(WORK + "/s1.ppm")
    print("after e4:", *b1, sep="\n  ")

    move(6, 7, 5, 5)                            # 2. Nf3
    send("screendump " + WORK + "/s2.ppm", 1.0)
    print("after Nf3 face:", face_counts(WORK + "/s2.ppm"))
    b2 = read_board(WORK + "/s2.ppm")
    print("after Nf3:", *b2, sep="\n  ")

    move(5, 5, 4, 3)                            # 3. Ne5?? blunder
    send("screendump " + WORK + "/s3.ppm", 1.0)
    print("after Ne5 face:", face_counts(WORK + "/s3.ppm"))
    b3 = read_board(WORK + "/s3.ppm")
    print("after Ne5?? :", *b3, sep="\n  ")

    # was our knight captured?  knight left f3, went to e5; if e5 is now
    # '.' with a black piece elsewhere or e5 == 'p' → captured.
    print("e5 row3:", b3[3][4], " f3 row5:", b3[5][5])
    send("quit", 0.4); qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass
