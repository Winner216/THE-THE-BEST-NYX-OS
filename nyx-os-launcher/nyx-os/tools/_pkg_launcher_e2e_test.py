#!/usr/bin/env python3
"""_pkg_launcher_e2e_test.py — installed packages appear in the App Library.

Boots the ISO with the host repo running, installs pkghello through the
Terminal, opens the App Library (desktop search "AppLib"), cycles the
category tabs to "Packages", and screendumps.  The pkghello card must
render with its magenta (color 5 = 0xAA00AA) icon box on screen.
"""
import os
import subprocess
import sys
import time

os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")

subprocess.run(["make", "pkgrepo"], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
store = subprocess.Popen(["python3", "tools/pkg_store.py"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.7)

WORK = "/tmp/pkglaunchtest"
os.makedirs(WORK, exist_ok=True)
SERIAL = os.path.join(WORK, "serial.log")
PPM = os.path.join(WORK, "shot.ppm")
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


def screendump(name):
    send("screendump " + name, 1.2)
    return os.path.exists(name)


def has_magenta_icon(path, expect=True):
    """Look for the 0xAA00AA icon box in the top-left card area."""
    try:
        with open(path, "rb") as f:
            head = f.readline().strip()
            if head not in (b"P6", b"P3"):
                return False
            while True:
                ln = f.readline().strip()
                if ln and not ln.startswith(b"#"):
                    break
            w, h = map(int, ln.split())
            if head == b"P6":
                f.readline()
                data = f.read()
            else:
                data = b""
        if head == b"P3":
            return False
        # Card icon box: window at (16,40), GY=158, icon at (34,168) 48x48
        found = 0
        for y in range(170, 215, 4):
            for x in range(36, 80, 4):
                off = (y * w + x) * 3
                r, g, b = data[off], data[off + 1], data[off + 2]
                if abs(r - 0xAA) < 25 and abs(g - 0x00) < 25 and abs(b - 0xAA) < 25:
                    found += 1
        return found > 8
    except Exception:
        return False


ok = False
try:
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: no login screen"); sys.exit(1)
    print("[1] login screen up")
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up"); sys.exit(1)
    print("[2] desktop up")

    print("[3] Terminal: pkg update + install pkghello")
    send("sendkey f1", 1.5)
    time.sleep(0.8)
    keys(list("pkg update"), 0.14)
    send("sendkey ret", 2.5)
    keys(list("pkg install pkghello"), 0.14)
    send("sendkey ret", 3.0)
    # Terminal output is screen-only; confirm the install by running it
    # (pkg_run prints [PKGHELLO] to COM1) — a run only works when the
    # ELF actually landed in the filesystem.
    keys(list("pkg run pkghello"), 0.14)
    send("sendkey ret", 0.5)
    if not wait_serial("[PKGHELLO] store package running!", 45):
        print("FAIL: install/run did not finish"); sys.exit(1)
    print("    installed + runs")

    print("[4] ESC to desktop, search AppLib (/ first — typing a bare 'p'")
    print("       would open the power menu instead!)")
    send("sendkey esc", 1.0)
    time.sleep(0.5)
    send("sendkey slash", 0.4)      # activate desktop search
    keys(list("applib"), 0.14)
    send("sendkey ret", 1.5)
    time.sleep(1.0)

    print("[5] TAB x7 -> Packages category")
    for _ in range(7):
        send("sendkey tab", 0.25)
    time.sleep(0.6)

    print("[6] screendump + pixel check")
    if screendump(PPM):
        if has_magenta_icon(PPM):
            print("    MAGENTA ICON FOUND — pkghello card is on screen")
            ok = True
        else:
            print("    no magenta icon in card area")
    else:
        print("    screendump failed")

finally:
    qemu.stdin.write("quit\n".encode())
    qemu.stdin.flush()
    qemu.wait(timeout=10)
    store.terminate()

print("\n" + ("PASS: installed package appears in the App Library launcher"
              if ok else "FAIL: package card not visible in launcher"))
sys.exit(0 if ok else 1)