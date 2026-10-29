#!/usr/bin/env python3
"""E2E: launch DOOM.EXE from the Files app and verify game mode works.

Flow: boot (KVM) → login → open Files → Local Disk → navigate to
DOOM.EXE (row 6) → Enter.  pe_exec maps the 4.8 MB PE at 0xC1000000,
DOOM's ring-3 entry calls SYS_GAMESTART which flips the desktop into
MODE_GAME.  Assertions:
  - serial shows the PE loader accepted DOOM.EXE ("PE: loading")
  - [DOOM] markers prove ring-3 execution reached nyx_main, GAMESTART
    succeeded and doomgeneric_Create is running
  - screendump during game mode differs from the pre-launch desktop
    (the game title screen rendered)
  - pressing keys doesn't crash the guest (screendumps keep arriving)
  - clicking the [X] (or ESC handling) is exercised only manually;
    the test kills QEMU at the end.
"""
import subprocess, time, os, sys, hashlib

WORK = "/tmp/doomtest"
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
     "-monitor", "stdio", "-serial", "file:" + WORK + "/serial.log",
     "-display", "none"],
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

def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()

results = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    results["desktop"] = wait_log("DESKTOP UP", 120)
    time.sleep(5)
    send("sendkey f2", 1.2)                            # Files (home)
    time.sleep(0.6)
    for _ in range(5): send("sendkey shift-b", 0.25)   # -> Local Disk tile
    send("sendkey ret", 0.8)                           # open Local Disk
    time.sleep(1.5)
    print("first sel:", sel_lines()[-1] if sel_lines() else None)
    # DOOM.EXE is Files row 8 (row 0 is the parent heading; embed order after that)
    results["nav_doom"] = nav_row(8, "DOOM.EXE")
    time.sleep(0.8)
    send("screendump " + WORK + "/pre_launch.ppm", 0.8)
    send("sendkey ret", 1.2)                           # launch the .exe
    # The PE loader prints its markers while mapping 4.8 MB.
    results["pe_load"] = wait_log("PE: loading", 60)
    # Ring-3 DOOM comes up: entry → zone → GAMESTART → doomgeneric_Create
    results["doom_start"] = wait_log("[DOOM] starting", 60)
    results["gamestart"] = wait_log("[DOOM] doomgeneric_Create", 90)
    # Give the title loop ~12 s to render frames.
    time.sleep(12)
    send("screendump " + WORK + "/doom_title.ppm", 0.8)
    # Exercise keys (fire/menu) and confirm the guest stays alive.
    for k in ("spc", "ret", "up", "down"):
        send("sendkey " + k, 0.5)
    time.sleep(4)
    send("screendump " + WORK + "/doom_after_keys.ppm", 0.8)
    time.sleep(2)
    results["alive"] = qemu.poll() is None
    send("quit", 0.4)
    qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass

log = readlog()
keep = ("PE:", "[DOOM]", "VDX", "map [", "stack VA", "GAME", "FATAL",
        "PANIC", "page fault", "GP ")
for l in log.splitlines():
    if any(k in l for k in keep):
        print("|", l[:150])
for n in ("pre_launch", "doom_title", "doom_after_keys"):
    p = WORK + "/" + n + ".ppm"
    if os.path.exists(p):
        print(n, "md5:", md5(p), os.path.getsize(p))

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

try:
    d1 = diff_ppm(WORK + "/pre_launch.ppm", WORK + "/doom_title.ppm")
    print("desktop→title changed pixels:", d1)
    results["rendered"] = d1 > 20000
except Exception as e:
    print("diff err:", e)
    results["rendered"] = False

try:
    d2 = diff_ppm(WORK + "/doom_title.ppm", WORK + "/doom_after_keys.ppm")
    print("title→after-keys changed pixels:", d2)
    results["keys_did_something"] = d2 > 500
except Exception as e:
    print("diff2 err:", e)
    results["keys_did_something"] = False

print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
ok = all(results.values())
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
