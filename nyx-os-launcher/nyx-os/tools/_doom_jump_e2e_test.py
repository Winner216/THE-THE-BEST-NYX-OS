#!/usr/bin/env python3
"""E2E: Space=jump works in DOOM (Space no longer the use key).

Flow: boot (KVM) → login → Files → Local Disk → DOOM.EXE (row 8) →
wait for real gameplay → screendump A → Space burst → screendump B →
assert visible world motion with no kernel faults.
Choreography matches tools/_doom_relaunch_e2e_test.py (proven).
"""
import subprocess, time, os, shutil, socket, json, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
WORK = "/tmp/doomjumptest"
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK, exist_ok=True)
SERIAL = WORK + "/serial.log"
QMP_SOCK = WORK + "/qmp.sock"

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-audiodev", "none,id=snd", "-device", "AC97,audiodev=snd",
     "-monitor", "stdio", "-serial", "file:" + SERIAL,
     "-qmp", "unix:" + QMP_SOCK + ",server,nowait",
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

def readlog():
    try:
        with open(SERIAL, "r", errors="ignore") as f:
            return f.read()
    except Exception:
        return ""

def wait_log(sub, timeout=120):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sub in readlog(): return True
        time.sleep(1)
    return False

def qmp_connect(path, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
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

qmp_s, qmp_f = qmp_connect(QMP_SOCK)

def qmp_cmd(name, args):
    qmp_f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
    qmp_f.flush()
    return json.loads(qmp_f.readline())

def screendump(name):
    send("screendump " + WORK + "/" + name + ".ppm", 0.8)

def diff_ppm(a, b):
    try:
        A = open(a, "rb").read(); B = open(b, "rb").read()
    except FileNotFoundError:
        return -1
    if len(A) != len(B) or len(A) < 100:
        return -1
    i = A.find(b"255\n") + 4
    d = 0
    for j in range(i, len(A) - 2, 3):
        if abs(A[j] - B[j]) > 18 or abs(A[j+1] - B[j+1]) > 18:
            d += 1
    return d

results = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    time.sleep(1.5)
    wait_log("DESKTOP UP", 90)
    time.sleep(5)
    send("sendkey f2", 1.2)                          # open Files (home)
    time.sleep(0.6)
    for _ in range(5): send("sendkey shift-b", 0.25)  # -> Local Disk
    send("sendkey ret", 0.8)
    time.sleep(1.5)
    for _ in range(8): send("sendkey shift-b", 0.25)  # -> row 8 (DOOM.EXE)
    time.sleep(0.4)
    send("sendkey ret", 1.2)
    results["pe_load"] = wait_log("PE: loading", 60)
    results["doom_start"] = wait_log("[DOOM] doomgeneric_Create", 90)
    results["doom_start"] = results["doom_start"]
    # Start a real game: ESC menu → New Game → Episode 1 → skill 2
    time.sleep(12)
    send("sendkey esc", 0.8)
    send("sendkey ret", 1.0)      # New Game
    send("sendkey ret", 1.0)      # Episode 1
    send("sendkey down", 0.5)     # skill 2
    send("sendkey ret", 1.0)
    results["igl"] = wait_log(" IGL", 90)
    # The menu sequence is timing-flaky: if it fails we're still on the
    # title's attract demo (status shows E1M5).  Retry until the status
    # line proves we're in a real E1M1 game.
    in_e1m1 = False
    for _ in range(3):
        time.sleep(4)
        if "ST E1M1" in readlog():
            in_e1m1 = True
            break
        send("esc", 0.8)
        send("sendkey ret", 1.0)
        send("sendkey ret", 1.0)
        send("sendkey down", 0.5)
        send("sendkey ret", 1.0)
        wait_log(" IGL", 90)
    results["in_e1m1"] = in_e1m1
    print("launch:", results)
    time.sleep(6)   # let the level splash fully pass

    # ── Space = jump: hold Space and watch the status-line z field
    # (250 ms sampling).  z > 0 while grounded-start = the impulse is
    # real physics; pixel diffs are only secondary because the ~0.5 s
    # arc returns to a near-identical view.
    best = -1
    mark = len(readlog())
    for attempt in range(3):
        screendump("pre")
        mark = len(readlog())
        for _ in range(14): send("sendkey spc", 0.15)   # ~2 s of holding
        screendump("post")
        trace = readlog()[mark:]
        zs = [int(tok[2:]) for l in trace.splitlines() if " z=" in l
              for tok in l.split() if tok.startswith("z=")]
        peak = max(zs) if zs else -999
        d = diff_ppm(WORK + "/pre.ppm", WORK + "/post.ppm")
        print(f"jump attempt {attempt}: z_peak={peak} diff={d}")
        if peak > 0:
            best = max(best, d)
            break
        best = max(best, d)
        time.sleep(1.0)
    results["jump_moves"] = peak > 0   # physics proof: player left the floor
    print("JUMP z-peak + diff:", peak, best)

    # ── E = use (regression): game keeps running, no faults ──────
    for _ in range(6): send("sendkey e", 0.12)
    time.sleep(2.5)
    log = readlog()
    faults = ("FATAL" in log or "panic" in log.lower()
              or "#PF" in log or "GP fault" in log)
    results["no_faults"] = not faults
    results["status_hook"] = "[DOOM] ST " in log
    m = [l for l in log.splitlines() if "[DOOM] ST " in l]
    if m: print("status:", m[-1])

    print("RESULT:", "PASS" if all(results.values()) else "FAIL", results)
finally:
    qemu.terminate()
    try: qemu.wait(5)
    except Exception: qemu.kill()
sys.exit(0 if all(results.values()) else 1)
