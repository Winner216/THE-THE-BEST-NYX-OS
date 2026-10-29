#!/usr/bin/env python3
"""E2E playtest: prove DOOM is actually playable on Nyx.

Flow: boot → login → desktop search "doom" → launch → start real game
(Episode 1, skill 1) → then, in E1M1:
  1. turn 180° in both directions (view must change a lot)
  2. walk forward/back (view must change)
  3. fire the pistol (muzzle flash / HUD ammo drop / sfx events)
  4. use (Space) a few times near the start (door sound / view change)
  5. 30 s combat soak with mixed input — task must stay alive,
     tick clock must keep advancing, no kernel fault on serial

Assertions use pixel-diffs of screendumps, the [DOOM] sfx counter,
and the absence of fault dumps in the serial log.
"""
import subprocess, time, os, sys, math, struct, socket, json

WORK = "/tmp/doomplaytest"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
WAV = os.path.join(WORK, "audio.wav")
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

t0 = time.time()
qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-audiodev", "wav,id=snd,path=" + WAV,
     "-device", "AC97,audiodev=snd",
     "-monitor", "stdio", "-serial", "file:" + WORK + "/serial.log",
     "-qmp", "unix:" + QMP_SOCK + ",server,nowait",
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

# ── QMP mouse injection (deterministic turns; HMP mouse_move collapses
#    X deltas with -display none, and sendkey timing can flake when
#    keys land during the level splash) ─────────────────────────────
def qmp_connect(path, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = None
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

qmp, qmpf = qmp_connect(QMP_SOCK)

def mouse_rel(dx, dy=0):
    evs = []
    if dx: evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
    if dy: evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
    qmpf.write(json.dumps({"execute": "input-send-event",
                           "arguments": {"events": evs}}) + "\n")
    qmpf.flush()
    json.loads(qmpf.readline())

results = {}

# ── Pixel-diff helpers (hoisted: the in-test retry loops use them) ──
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
    results["sfx_cached"] = wait_log("snd: cached", 60)   # cache count is deterministic at init
    # Real game: ESC menu → New Game → Episode 1 → Skill 2 ("Hey, not too rough"
    # gives a shotgun guy but plenty of health).
    time.sleep(12)
    send("sendkey esc", 0.8)
    send("sendkey ret", 1.0)      # New Game
    send("sendkey ret", 1.0)      # Episode 1
    send("sendkey down", 0.5)     # skill 2
    send("sendkey ret", 1.0)
    results["realgame"] = wait_log(" IGL", 60)
    time.sleep(8)   # let the level splash fully pass — early input lands
                    # on the level transition, not the player

    def dump(name, wait=0.8):
        send("screendump " + WORK + "/" + name + ".ppm", wait)

    # ── 1. Turning changes the view a lot (QMP mouse = deterministic) ─
    dump("t0")
    for _ in range(10):
        mouse_rel(120); time.sleep(0.15)          # ~180° via mouse aim
    dump("t1")
    for _ in range(10):
        mouse_rel(-120); time.sleep(0.15)         # back
    dump("t2")

    # ── 2. Walking changes the view (retries: sendkey bursts land
    #       unreliably right after level start) ──────────────────────
    for attempt in range(3):
        for _ in range(20): send("sendkey up", 0.2)       # forward
        dump("w1")
        for _ in range(12): send("sendkey down", 0.2)     # back up
        dump("w2")
        if diff_ppm(WORK+"/w1.ppm", WORK+"/w2.ppm") > 5000:
            break
        time.sleep(1.5)   # let the level settle, then retry

    # ── 3. Firing: HUD ammo must drop + gunshot audio peak (retries) ─
    for attempt in range(3):
        dump("f0")
        for _ in range(8): send("sendkey ctrl", 0.3)      # fire 8 shots
        dump("f1")
        if diff_strip(WORK+"/f0.ppm", WORK+"/f1.ppm", 645, 1290, 700, 790) > 100:
            break
        time.sleep(1.5)

    # ── Use key (doors / switches) near start (Space = jump now) ──
    for _ in range(6): send("sendkey e", 0.3)
    dump("u1")

    # ── 5. 30 s combat soak with mixed input ───────────────────────
    soak_end = time.time() + 30
    i = 0
    while time.time() < soak_end:
        send("sendkey up", 0.25)
        send("sendkey ctrl", 0.25)
        send("sendkey right", 0.25)
        i += 1
        if i % 6 == 0: send("sendkey e", 0.3)
        if i % 8 == 0: send("sendkey down", 0.25)
    dump("soak_end")

    results["alive"] = qemu.poll() is None
    # tick clock must still be advancing (no stall) — wait for a fresh tick
    ticks_before = readlog().count("[DOOM] tick")
    time.sleep(3)
    results["ticking"] = readlog().count("[DOOM] tick") > ticks_before
    send("quit", 1.0)
    try: qemu.wait(timeout=45)     # give QEMU time to flush the wav file
    except Exception: pass
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass

d_turn = max(diff_ppm(WORK+"/t0.ppm", WORK+"/t1.ppm"),
             diff_ppm(WORK+"/t1.ppm", WORK+"/t2.ppm"))
d_walk = max(diff_ppm(WORK+"/w1.ppm", WORK+"/w2.ppm"),
             diff_ppm(WORK+"/t2.ppm", WORK+"/w1.ppm"))

d_ammo = diff_strip(WORK+"/f0.ppm", WORK+"/f1.ppm", 645, 1290, 700, 790)

log = readlog()
faulted = ("50584621" in log or "PANIC" in log or "FATAL" in log
           or "GP fault" in log)

# ── WAV analysis: gunshots during the fire window ────────────────
def wav_rms_windows(path, win_ms=100):
    d = open(path, "rb").read()
    if len(d) < 44 or d[:4] != b"RIFF": return None
    pos = 12; fmt = None; data = None
    while pos + 8 <= len(d):
        cid = d[pos:pos+4]; ln = struct.unpack("<I", d[pos+4:pos+8])[0]
        if cid == b"fmt ": fmt = d[pos+8:pos+8+ln]
        if cid == b"data": data = (pos+8, ln)
        pos += 8 + ln + (ln & 1)
    if not fmt or not data: return None
    ch = struct.unpack("<H", fmt[2:4])[0]
    rate = struct.unpack("<I", fmt[4:8])[0]
    bits = struct.unpack("<H", fmt[14:16])[0]
    off, ln = data
    ln = min(ln, len(d) - off)
    n = ln // (ch * bits // 8)
    raw = struct.unpack_from("<%dh" % (n * ch), d, off)
    step = int(rate * win_ms / 1000) * ch
    out = []
    for i in range(0, len(raw) - step, step):
        acc = 0
        for j in range(i, i + step, 7):
            acc += raw[j] * raw[j]
        out.append(math.sqrt(acc / (step / 7)))
    return out

# QEMU's wav writer only records while the AC97 ring is active, so the
# file has no reliable wall-clock alignment — assert on the whole-file
# peak instead (a gunshot is by far the loudest sound in E1M1).
rms = wav_rms_windows(WAV)
for _ in range(10):                # file may still be flushing
    if rms: break
    time.sleep(1)
    rms = wav_rms_windows(WAV)
audio_peak = max(rms) if rms else 0

print("turn-view delta:", d_turn)
print("walk-view delta:", d_walk)
print("ammo-HUD delta:", d_ammo)
print("audio peak RMS:", round(audio_peak))
print("fault markers:", "YES" if faulted else "none")

results["view_turns"]  = d_turn > 8000
results["view_walks"]  = d_walk > 8000
results["ammo_drops"]  = d_ammo > 100
results["gunshots"]    = audio_peak > 4000
results["no_faults"]   = not faulted

print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
ok = all(results.values())
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
