#!/usr/bin/env python3
"""_pkg_e2e_test.py — Nyx App Store end-to-end test.

Boots the ISO with the host repo running (tools/pkg_store.py on the
host, port 8123), logs in, opens the Terminal and exercises the
package manager:

  1. `pkg update`        — pull the catalog from 10.0.2.2:8123
  2. `pkg install pkghello` — download + store the ring-3 ELF
  3. `pkg run pkghello`  — launch it; its sys_write output lands on
                          COM1 ([PKGHELLO] ... pid= ..., ticks, exit)
  4. same for pkgquote   — a second catalog app ([PKGQUOTE] ...)

The [PKG*] markers prove the whole chain: guest HTTP GET to the host
repo -> filesystem install -> user_exec ring-3 launch -> syscalls.
"""
import os
import subprocess
import sys
import time

os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")

# Make sure the repo has fresh ELFs staged, then serve it.
subprocess.run(["make", "pkgrepo"], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
store = subprocess.Popen(["python3", "tools/pkg_store.py"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.7)

WORK = "/tmp/pkgtest"
os.makedirs(WORK, exist_ok=True)
SERIAL = os.path.join(WORK, "serial.log")
open(SERIAL, "w").close()

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


def keys(seq, gap=0.16):
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


ok = True
try:
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: no login screen"); sys.exit(1)
    print("[1] login screen up")
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up"); sys.exit(1)
    print("[2] desktop up")

    print("[3] F1 -> Terminal, `pkg update` ...")
    send("sendkey f1", 1.5)
    time.sleep(0.8)
    keys(list("pkg update"), 0.14)
    send("sendkey ret", 2.5)

    print("[4] `pkg install pkghello` ...")
    keys(list("pkg install pkghello"), 0.14)
    send("sendkey ret", 3.0)

    print("[5] `pkg run pkghello` ...")
    keys(list("pkg run pkghello"), 0.14)
    send("sendkey ret", 0.5)
    if not wait_serial("[PKGHELLO] store package running! pid=", 45):
        print("FAIL: pkghello never ran (update/install/run chain broken?)")
        ok = False
    else:
        print("    pkghello launched from the store")
    if not wait_serial("[PKGHELLO] exiting via sys_exit", 30):
        print("NOTE: pkghello exit marker not seen (timing)")
    else:
        print("    pkghello finished cleanly")

    print("[6] install + run second package `pkgquote` ...")
    keys(list("pkg install pkgquote"), 0.14)
    send("sendkey ret", 3.0)
    keys(list("pkg run pkgquote"), 0.14)
    send("sendkey ret", 0.5)
    if not wait_serial("[PKGQUOTE]", 45):
        print("FAIL: pkgquote never ran")
        ok = False
    else:
        print("    pkgquote launched from the store")

    with open(SERIAL, "r", errors="ignore") as f:
        log = f.read()
    for bad in ("DNS failed", "ELF load failed", "not installed",
                "download failed", "no network"):
        if bad in log:
            print("FAIL: error marker in log: %r" % bad)
            ok = False

finally:
    qemu.stdin.write("quit\n".encode())
    qemu.stdin.flush()
    qemu.wait(timeout=10)
    store.terminate()

print("\n" + ("PASS: App Store install+run verified"
              if ok else "FAIL: package manager checks failed"))
sys.exit(0 if ok else 1)
