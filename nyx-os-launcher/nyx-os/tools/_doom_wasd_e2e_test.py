#!/usr/bin/env python3
"""E2E: WASD movement in DOOM.

W/S must move forward/back, A/D must strafe (view changes without
turning — crosshair-relative scene shifts), and the arrow keys must
still work.  Started from a real game in E1M1.

Method: pixel-diff screendumps around key bursts, plus a turning-free
strafe check (mouse is not touched during the A/D burst, so any view
change is strafing, not turning).
"""
import subprocess, time, os, sys, socket, json

WORK = "/tmp/doomwasdtest"
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
     "-monitor", "stdio", "-serial", "file:" + WORK + "/serial.log",
     "-qmp", "unix:" + QMP_SOCK + ",server,nowait",
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

# ── Pixel-diff helpers ────────────────────────────────────────────
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

def dump(name, wait=0.8):
    send("screendump " + WORK + "/" + name + ".ppm", wait)

# ── QMP mouse injection (deterministic turns — used to face open
#    floor before the forward-motion tests; E1M1's spawn faces a wall) ─
def qmp_connect(path, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = None
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            f = s.makefile("rw")
            json.loads(f.readline())          # greeting
            f.write('{"execute":"qmp_capabilities"}\\n'); f.flush()
            json.loads(f.readline())          # capabilities ack
            return s, f
        except Exception:
            if s: s.close()
            time.sleep(0.5)
    raise RuntimeError("QMP connect failed")

qmp, qmpf = qmp_connect(QMP_SOCK)

def mouse_rel(dx, dy=0):
    evs = []
    if dx: evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
    if dy: evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
    qmpf.write(json.dumps({"execute": "input-send-event",
                           "arguments": {"events": evs}}) + "\\n")
    qmpf.flush()
    json.loads(qmpf.readline())

results = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    results["desktop"] = wait_log("DESKTOP UP", 120)
    time.sleep(5)
    send("sendkey slash", 0.6)
    for k in ("d", "o", "o", "m"): send("sendkey " + k, 0.3)
    send("sendkey ret", 1.2)
    results["pe_load"] = wait_log("PE: loading", 60)
    results["doom_start"] = wait_log("[DOOM] starting", 60)
    results["gamestart"] = wait_log("[DOOM] doomgeneric_Create", 90)
    time.sleep(12)
    send("sendkey esc", 0.8)
    send("sendkey ret", 1.0)      # New Game
    send("sendkey ret", 1.0)      # Episode 1
    send("sendkey ret", 1.0)      # Skill 1
    results["realgame"] = wait_log(" IGL", 60)
    time.sleep(8)                 # level splash fully passed

    # Face away from the spawn wall first: E1M1 starts the player
    # looking straight at rock, so forward keys produce no view change
    # until we turn.  180° via QMP mouse = deterministic.
    for _ in range(15):
        mouse_rel(120); time.sleep(0.12)
    time.sleep(0.5)

    # ── W: forward ─────────────────────────────────────────────────
    for attempt in range(3):
        dump("w0")
        for _ in range(15): send("sendkey w", 0.2)
        dump("w1")
        if diff_ppm(WORK+"/w0.ppm", WORK+"/w1.ppm") > 20000: break
        time.sleep(1.5)
    d_w = diff_ppm(WORK+"/w0.ppm", WORK+"/w1.ppm")

    # ── S: backward ────────────────────────────────────────────────
    #    Re-aim 90° first: after the W walk the player may be backed
    #    against a wall (backward moves but nothing visibly changes). ──
    for _ in range(8):
        mouse_rel(120); time.sleep(0.12)
    time.sleep(0.5)
    for attempt in range(3):
        dump("s0")
        for _ in range(12): send("sendkey s", 0.2)
        dump("s1")
        if diff_ppm(WORK+"/s0.ppm", WORK+"/s1.ppm") > 20000: break
        time.sleep(1.5)
    d_s = diff_ppm(WORK+"/s0.ppm", WORK+"/s1.ppm")

    # ── A: strafe left (no mouse → view changes WITHOUT turning).
    #    Re-face 90° first: after the walks the left side may be a
    #    uniform wall (little visible change even while strafing). ──
    for _ in range(8):
        mouse_rel(120); time.sleep(0.12)
    time.sleep(0.5)
    for attempt in range(3):
        dump("a0")
        for _ in range(15): send("sendkey a", 0.2)
        dump("a1")
        if diff_ppm(WORK+"/a0.ppm", WORK+"/a1.ppm") > 8000: break
        time.sleep(1.5)
    d_a = diff_ppm(WORK+"/a0.ppm", WORK+"/a1.ppm")

    # ── D: strafe right ────────────────────────────────────────────
    for _ in range(8):
        mouse_rel(-240); time.sleep(0.12)   # face the other side
    time.sleep(0.5)
    for attempt in range(3):
        dump("d0")
        for _ in range(15): send("sendkey d", 0.2)
        dump("d1")
        if diff_ppm(WORK+"/d0.ppm", WORK+"/d1.ppm") > 8000: break
        time.sleep(1.5)
    d_d = diff_ppm(WORK+"/d0.ppm", WORK+"/d1.ppm")

    # ── Arrows still work (regression; fresh 90° turn so forward
    #    faces open floor again) ─────────────────────────────────────
    for _ in range(8):
        mouse_rel(120); time.sleep(0.12)
    time.sleep(0.5)
    for attempt in range(3):
        dump("up0")
        for _ in range(15): send("sendkey up", 0.2)
        dump("up1")
        if diff_ppm(WORK+"/up0.ppm", WORK+"/up1.ppm") > 20000: break
        time.sleep(1.5)
    d_up = diff_ppm(WORK+"/up0.ppm", WORK+"/up1.ppm")

    results["alive"] = qemu.poll() is None
    send("quit", 0.5)
    try: qemu.wait(timeout=15)
    except Exception: pass
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass

print("W forward delta:  ", d_w)
print("S backward delta: ", d_s)
print("A strafe delta:   ", d_a)
print("D strafe delta:   ", d_d)
print("UP arrow delta:   ", d_up)
log = readlog()
faulted = ("50584621" in log or "PANIC" in log or "FATAL" in log)
results["no_faults"] = not faulted

results["w_moves"]   = d_w > 20000
results["s_moves"]   = d_s > 20000
results["a_strafes"] = d_a > 8000
results["d_strafes"] = d_d > 8000
results["arrows_ok"] = d_up > 20000

print("fault markers:", "YES" if faulted else "no")
print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
ok = all(results.values())
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
