#!/usr/bin/env python3
"""E2E: launch CHESS.EXE from the Files app and verify the chess game runs.

Flow: boot (KVM) → login → Files → Local Disk → CHESS.EXE → Enter.
Assertions (serial):
  - "PE: loading"           — the PE loader accepted CHESS.EXE
  - "[CHESS] up"            — ring-3 entry ran, canvas claimed
  - guest stays alive after keyboard input (no faults)
Kills QEMU at the end.
"""
import subprocess, time, os, sys, hashlib

WORK = "/tmp/chesstest"
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
     "-monitor", "stdio", "-serial", "file:" + serial_path,
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

def nav_to_name(target_name, label):
    """Walk the Local Disk list until SEL shows the target file.
    The view is name-ordered and selection starts on row 0 (heading,
    which emits no SEL marker) — the first shift-b lands on row 1 and
    markers flow from there."""
    guard = 0
    if not step("shift-b"):                 # row 0 → row 1
        print(label, "no SEL after first shift-b")
        return False
    while guard < 90:
        s = sel_lines()
        seen = ""
        if s:
            parts = s[-1].split(None, 2)
            seen = parts[2] if len(parts) >= 3 else ""
            if seen == target_name:
                print(label, "at", seen)
                return True
        if seen and seen < target_name:
            if not step("shift-b"): break
        else:
            if not step("shift-a"): break
        guard += 1
    s = sel_lines()
    print(label, "stuck at", s[-1] if s else None)
    return False

def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()

results = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    results["desktop"] = wait_log("DESKTOP UP", 120)
    time.sleep(12)                                     # wallpaper/icons settle
    send("screendump " + WORK + "/desktop.ppm", 0.8)
    send("sendkey f2", 1.2)                            # Files (home)
    time.sleep(0.6)
    send("screendump " + WORK + "/files.ppm", 0.8)
    for _ in range(5): send("sendkey shift-b", 0.25)   # -> Local Disk tile
    send("sendkey ret", 0.8)                           # open Local Disk
    time.sleep(1.5)
    results["nav_chess"] = nav_to_name("CHESS.EXE", "CHESS.EXE")
    time.sleep(0.8)
    send("screendump " + WORK + "/pre_launch.ppm", 0.8)
    send("sendkey ret", 1.2)                           # launch the .exe
    results["pe_load"] = wait_log("PE: loading", 60)
    results["chess_up"] = wait_log("[CHESS] up", 60)
    time.sleep(6)                                      # title/first frames
    send("screendump " + WORK + "/chess_board.ppm", 0.8)

    # Exercise keys: moves shouldn't crash the guest.
    for k in ("spc", "ret", "n", "z", "u"):
        send("sendkey " + k, 0.5)
    time.sleep(4)
    send("screendump " + WORK + "/chess_after_keys.ppm", 0.8)
    time.sleep(2)
    results["alive"] = qemu.poll() is None

    # Diffs: board renders (differs from desktop) and keys changed pixels.
    def diff_ppm(a, b):
        def openit(p):
            d = open(p, "rb").read()
            i = 0
            while d[i:i+1] != b"\n": i += 1
            i += 1
            while d[i:i+1] != b"\n": i += 1
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

    d1 = diff_ppm(WORK + "/pre_launch.ppm", WORK + "/chess_board.ppm")
    d2 = diff_ppm(WORK + "/chess_board.ppm", WORK + "/chess_after_keys.ppm")
    print("desktop→board changed px:", d1, " board→after-keys:", d2)
    results["rendered"] = d1 > 20000
    results["keys_did_something"] = d2 > 200

    send("quit", 0.4)
    qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass

log = readlog()
keep = ("PE:", "[CHESS]", "FATAL", "PANIC", "page fault", "GP ")
for l in log.splitlines():
    if any(k in l for k in keep):
        print("|", l[:150])

print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
ok = all(results.values())
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
