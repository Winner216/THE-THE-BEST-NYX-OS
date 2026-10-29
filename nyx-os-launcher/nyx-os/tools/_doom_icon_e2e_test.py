#!/usr/bin/env python3
"""E2E: launch DOOM from the desktop icon (search "doom" → Enter).

Flow: boot (KVM) → login → press '/' (desktop search) → type "doom" →
Enter.  do_launch(19) finds DOOM.EXE in the embedded filesystem and
pe_exec()s it.  Assertions:
  - serial shows the PE loader accepted DOOM.EXE
  - [DOOM] markers prove ring-3 execution + doomgeneric_Create
  - screendump during game mode differs from the pre-launch desktop
  - keys keep the guest alive and change the screen
  - sound: kernel mixer present, sfx cache uploads complete
"""
import subprocess, time, os, sys, hashlib

WORK = "/tmp/doomicontest"
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

def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()

results = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    results["desktop"] = wait_log("DESKTOP UP", 120)
    time.sleep(5)
    # Desktop search: '/' opens it, typing filters, Enter launches.
    send("sendkey slash", 0.6)                # '/' (unshifted on US layout)
    send("sendkey d", 0.3); send("sendkey o", 0.3)
    send("sendkey o", 0.3); send("sendkey m", 0.3)
    time.sleep(0.5)
    send("screendump " + WORK + "/search.ppm", 0.8)
    send("sendkey ret", 1.2)                  # launch Doom
    results["pe_load"] = wait_log("PE: loading", 60)
    results["doom_start"] = wait_log("[DOOM] starting", 60)
    results["gamestart"] = wait_log("[DOOM] doomgeneric_Create", 90)
    results["mixer"] = wait_log("kernel mixer present", 30)
    # Let the title loop render and the sfx cache upload.
    time.sleep(15)
    send("screendump " + WORK + "/doom_title.ppm", 0.8)
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
keep = ("PE:", "[DOOM]", "GS:", "GAME", "FATAL", "PANIC",
        "page fault", "GP ", "PFX")
for l in log.splitlines():
    if any(k in l for k in keep):
        print("|", l[:150])
for n in ("search", "doom_title", "doom_after_keys"):
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
    d1 = diff_ppm(WORK + "/search.ppm", WORK + "/doom_title.ppm")
    print("search→title changed pixels:", d1)
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

# sfx cache marker (uploads completed without a ring-3 fault)
results["sfx_cached"] = "snd: cached" in log and "snd: cached 0" not in log

print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
ok = all(results.values())
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
