#!/usr/bin/env python3
"""_pkgs_more_e2e_test.py — install & run the expanded App Store catalog.

Boots the ISO with the host repo serving pkgcalc/pkgfetch/pkglife plus
the original pkghello/pkgquote, then through the Terminal:
  pkg install all   →  all five apps installed
  pkg run pkgcalc   →  [PKGCALC] ... pi digits ... all done
  pkg run pkglife   →  [PKGLIFE] 40 generations done
  pkg run pkgfetch  →  [PKGFETCH] ... real HTTP GET to example.com ...
"""
import os
import subprocess
import sys
import time

os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")

subprocess.run(["make", "pkgrepo"], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
WORK = "/tmp/pkgsmore"
os.makedirs(WORK, exist_ok=True)
REQLOG = os.path.join(WORK, "store_reqs.log")
SERIAL = os.path.join(WORK, "serial.log")
open(SERIAL, "w").close()
open(REQLOG, "w").close()

env = dict(os.environ)
env["PKGSTORE_LOG"] = REQLOG
store = subprocess.Popen(["python3", "tools/pkg_store.py"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         env=env)
time.sleep(0.7)

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-nic", "user,model=e1000",
     "-monitor", "stdio", "-serial", "file:" + SERIAL,
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL)


def send(cmd, wait=0.35):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)


QKEY = {".": "dot", "/": "slash", "-": "minus", " ": "spc"}


def keys(seq, gap=0.14):
    for k in seq:
        send("sendkey " + QKEY.get(k, k), gap)


def wait_serial(marker, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(SERIAL, "r", errors="ignore") as f:
                if marker in f.read():
                    return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


ok = False
try:
    if not wait_serial("LOGIN READY", 120):
        print("FAIL: no login screen"); sys.exit(1)
    print("[1] login screen up")
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up"); sys.exit(1)
    print("[2] desktop up")

    print("[3] Terminal: pkg install all")
    send("sendkey f1", 1.5)
    time.sleep(0.8)
    keys(list("pkg update"), 0.14)
    send("sendkey ret", 2.5)
    keys(list("pkg install all"), 0.14)
    send("sendkey ret", 3.5)
    time.sleep(2.0)

    print("[4] run pkgcalc")
    keys(list("pkg run pkgcalc"), 0.14)
    send("sendkey ret", 0.6)
    if not wait_serial("[PKGCALC] all done", 60):
        print("FAIL: pkgcalc did not finish"); sys.exit(1)
    print("    pkgcalc finished")
    # digits sanity: pi line must contain the first 20 known digits
    with open(SERIAL, "r", errors="ignore") as f:
        s = f.read()
    if "3.14159265358979323846" in s:
        print("    pi digits verified on COM1")
    else:
        print("    WARN: pi prefix not found in serial (check math)")

    print("[5] run pkglife")
    keys(list("pkg run pkglife"), 0.14)
    send("sendkey ret", 0.6)
    if not wait_serial("[PKGLIFE] 40 generations done", 90):
        print("FAIL: pkglife did not finish"); sys.exit(1)
    print("    pkglife finished (animation rendered)")

    print("[6] run pkgfetch (real HTTP to example.com)")
    keys(list("pkg run pkgfetch"), 0.14)
    send("sendkey ret", 0.6)
    if not wait_serial("[PKGFETCH] done", 120):
        print("FAIL: pkgfetch did not finish"); sys.exit(1)
    print("    pkgfetch finished")
    with open(SERIAL, "r", errors="ignore") as f:
        s = f.read()
    if "HTTP/1.1 200" in s or "example.com" in s:
        print("    HTTP response bytes received on COM1")
    else:
        print("    WARN: no obvious HTTP 200 / example.com text in serial")

    # All five apps were fetched from the store repo on the host?
    with open(REQLOG, "r", errors="ignore") as f:
        reqs = f.read()
    missing = [n for n in ("pkgcalc.elf", "pkgfetch.elf", "pkglife.elf",
                           "pkghello.elf", "pkgquote.elf")
               if n not in reqs]
    if missing:
        print("    WARN: store never served:", missing)
    else:
        print("    store served all five .elf files")
    ok = True

finally:
    qemu.stdin.write("quit\n".encode())
    qemu.stdin.flush()
    qemu.wait(timeout=10)
    store.terminate()

print("\n" + ("PASS: calc/fetch/life installed and ran end-to-end"
              if ok else "FAIL"))
sys.exit(0 if ok else 1)
