#!/usr/bin/env python3
"""E2E: DOOM [X] close button.

Boot → login → Files → DOOM.EXE → Enter (game mode up, demo running)
→ move the mouse to the [X] box on the game title bar and click
→ assert: mode returns to Files (desktop visible), game task gone.
"""
import subprocess, time, os, shutil, hashlib, re, socket, json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
WORK = "/tmp/doomclosetest"
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK, exist_ok=True)
SERIAL = WORK + "/serial.log"
QMP_SOCK = WORK + "/qmp.sock"

def readlog():
    try:
        with open(SERIAL, "r", errors="ignore") as f:
            return f.read()
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
     "-monitor", "stdio",     "-serial", "file:" + SERIAL,
     "-display", "none",
     "-qmp", "unix:" + QMP_SOCK + ",server,nowait"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

def step(key):
    before = len(sel_lines())
    send("sendkey " + key, 0.2)
    dl = time.time() + 4
    while time.time() < dl:
        if len(sel_lines()) > before: return True
        time.sleep(0.15)
    return False

def nav_row(target, label):
    cur = last_row()
    if cur is None: cur = 0
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

def move_mouse(dx, dy):
    sx, sy = (1, -1) if dx > 0 else (-1, 1)
    for _ in range(abs(dx)):
        send("mouse_move %d 0" % sx, 0.012)
    sy2, sx2 = (1, -1) if dy > 0 else (-1, 1)
    for _ in range(abs(dy)):
        send("mouse_move 0 %d" % sy2, 0.012)

# ── QMP mouse (HMP mouse_move is degenerate with -display none) ──
def qmp_connect(path, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = None
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            f = s.makefile("rw")
            json.loads(f.readline())                       # greeting
            f.write('{"execute":"qmp_capabilities"}\n'); f.flush()
            json.loads(f.readline())                       # ack
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
    qmpf = None

def qmp_send(events):
    if not qmpf: return
    qmpf.write(json.dumps({"execute": "input-send-event",
                           "arguments": {"events": events}}) + "\n")
    qmpf.flush()
    try: json.loads(qmpf.readline())
    except Exception: pass

def mouse_to(tx, ty, cur):
    # PRECISE movement: the kernel applies 60% rounding to deltas larger
    # than its precision threshold, so big chunks undershoot — send ≤2
    # unit deltas for exact 1:1 cursor travel (needed for the 16×16 box).
    dx = tx - cur[0]; dy = ty - cur[1]
    while dx or dy:
        sx = max(-1, min(1, dx)); sy = max(-1, min(1, dy))   # ±1 only: MOUSE_PRECISION_MAX=1
        evs = []
        if sx: evs.append({"type": "rel", "data": {"axis": "x", "value": sx}})
        if sy: evs.append({"type": "rel", "data": {"axis": "y", "value": sy}})
        qmp_send(evs)
        dx -= sx; dy -= sy
        time.sleep(0.005)
    cur[0], cur[1] = tx, ty
    time.sleep(0.3)

def click(tx, ty, cur):
    mouse_to(tx, ty, cur)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.15)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.45)

results = {}
cur = [960, 540]
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    time.sleep(1.5)
    wait_log("DESKTOP UP", 90)                      # desktop fully ready
    time.sleep(5)
    send("sendkey f2", 1.2)                         # open Files (home)
    time.sleep(0.6)
    for _ in range(5): send("sendkey shift-b", 0.25)  # -> Local Disk tile
    send("sendkey ret", 0.8)                        # open Local Disk
    time.sleep(1.5)
    send("screendump " + WORK + "/before_nav.ppm", 0.8)
    print("sel lines so far:", len(sel_lines()))
    results["nav_doom"] = nav_row(8, "nav")
    send("sendkey ret", 1.2)                        # launch
    results["pe_load"] = wait_log("PE: loading", 60)
    results["doom_start"] = wait_log("[DOOM] doomgeneric_Create", 90)
    time.sleep(10)                                  # let the demo run

    # [X] box: DOOM canvas (640×400) at screen (640,340); the 24 px
    # title bar sits above it → box ≈ (1258..1274, 320..336); click its
    # center (1266, 328).
    click(1266, 328, cur)
    time.sleep(2.5)

    # Screenshot after close
    send("screendump " + WORK + "/after_close.ppm", 0.8)
    results["alive"] = qemu.poll() is None
    send("quit", 0.4)
    qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass

log = readlog()
keep = ("PE:", "[DOOM]", "GAME", "FATAL", "PANIC", "page fault")
for l in log.splitlines():
    if any(k in l for k in keep):
        print("|", l[:140])

# Verify: after_close.ppm shows the Files window, not the game canvas.
# Heuristic: the DOOM title screen's fire band (red-dominant rows) must
# be gone; the Files window's chrome is gray/white-ish.
try:
    d = open(WORK + "/after_close.ppm", "rb").read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', d, re.S)
    w, h = int(m.group(1)), int(m.group(2)); raw = d[m.end():]
    reds = 0; total = 0
    for y in range(330, 380, 2):        # fire band area
        for x in range(660, 1260, 4):
            o = (y * w + x) * 3
            r, g, b = raw[o], raw[o+1], raw[o+2]
            total += 1
            if r > g + 30 and r > b + 30 and r > 90: reds += 1
    frac = reds / max(total, 1)
    print(f"fire-band red fraction after close: {frac:.3f}")
    results["game_closed"] = frac < 0.02
except Exception as e:
    print("ppm err:", e); results["game_closed"] = False

print("RESULTS:", results)
ok = (results.get("login") and results.get("nav_doom") and
      results.get("doom_start") and results.get("alive") and
      results.get("game_closed"))
print("OVERALL:", "PASS" if ok else "FAIL")
sys_exit = 0 if ok else 1
raise SystemExit(sys_exit)
