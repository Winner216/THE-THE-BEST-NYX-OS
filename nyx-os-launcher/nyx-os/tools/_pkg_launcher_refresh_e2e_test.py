#!/usr/bin/env python3
"""_pkg_launcher_refresh_e2e_test.py — launcher refreshes without reopening.

Opens the App Library FIRST and TABs to the empty Packages category,
switches to the Terminal, installs pkghello, then Alt+Tabs back to the
already-open App Library (which does NOT re-run applib_init — the
refresh must come from the post-install applib_refresh() + per-frame
rescan).  The pkghello card's magenta icon must be on screen, on the
same category it was on before switching away.
"""
import os
import subprocess
import sys
import time

os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")

WORK = "/tmp/pkgrefresh"
os.makedirs(WORK, exist_ok=True)
SERIAL = os.path.join(WORK, "serial.log")
PPM = os.path.join(WORK, "shot.ppm")
STORELOG = os.path.join(WORK, "store.log")
open(SERIAL, "w").close()
open(STORELOG, "w").close()

subprocess.run(["make", "pkgrepo"], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
store = subprocess.Popen(["python3", "tools/pkg_store.py"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         env=dict(os.environ, PKGSTORE_LOG=STORELOG))
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


def has_magenta_icon(path):
    """0xAA00AA package icon tile anywhere in the library content area.

    The card's grid position varies with the library's persisted state
    (selection/scroll), so scan the whole content region instead of a
    fixed top-left box.  A real icon tile is ~48x48px, so require a
    solid magenta block (>= 40 sampled pixels inside a 40x40 window).
    Packages category never lists built-in apps, so any magenta tile
    there is an installed package.
    """
    try:
        with open(path, "rb") as f:
            head = f.readline().strip()
            if head != b"P6":
                return False
            while True:
                ln = f.readline().strip()
                if ln and not ln.startswith(b"#"):
                    break
            w, h = map(int, ln.split())
            f.readline()
            data = f.read()
        for y0 in range(120, 980, 4):
            for x0 in range(20, w - 60, 4):
                found = 0
                for yy in range(y0, y0 + 40, 4):
                    for xx in range(x0, x0 + 40, 4):
                        off = (yy * w + xx) * 3
                        r, g, b = data[off], data[off + 1], data[off + 2]
                        if abs(r - 0xAA) < 25 and abs(g - 0x00) < 25 and abs(b - 0xAA) < 25:
                            found += 1
                if found > 40:
                    return True
        return False
    except Exception:
        return False


def open_applib():
    """Desktop search (/ then 'applib') -> ENTER opens the library."""
    send("sendkey slash", 0.4)
    keys(list("applib"), 0.14)
    send("sendkey ret", 1.5)
    time.sleep(1.0)


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

    # Open the library FIRST — before any package exists — and TAB to
    # the Packages category.  Packages never lists built-in apps, so an
    # empty Packages view is a clean negative baseline (no magenta).
    print("[3] open App Library first, TAB to empty Packages")
    open_applib()
    for _ in range(7):
        send("sendkey tab", 0.25)
    time.sleep(0.6)
    send("screendump " + PPM, 1.2)
    if has_magenta_icon(PPM):
        print("    WARN: magenta icon already present before install?!")
    else:
        print("    empty Packages — no card yet (expected)")

    # F1 opens the Terminal OVER the library (library stays in the
    # window MRU at position 1, still on the Packages category), then
    # install a package.
    print("[4] F1 Terminal (over library): pkg update + install pkghello")
    send("sendkey f1", 1.5)
    time.sleep(0.8)
    keys(list("pkg update"), 0.14)
    send("sendkey ret", 2.5)
    keys(list("pkg install pkghello"), 0.14)
    send("sendkey ret", 3.0)
    keys(list("pkg run pkghello"), 0.14)
    send("sendkey ret", 0.5)
    if not wait_serial("[PKGHELLO] store package running!", 45):
        print("FAIL: install/run did not finish")
        try:
            print("    store requests seen by host:")
            for ln in open(STORELOG):
                print("      " + ln.rstrip())
        except OSError:
            pass
        sys.exit(1)
    print("    installed + runs")

    # Alt+Tab ONCE: MRU=[TERMINAL, APPLIB] -> commit goes to the
    # already-open App Library WITHOUT applib_init().  The library
    # keeps its state (still the Packages category).  QEMU's sendkey
    # chord holds alt+tab then releases; 800ms reliably delivers the
    # break (longer holds drop it).
    print("[5] Alt+Tab back to the already-open App Library")
    send("sendkey alt-tab 800", 2.5)
    time.sleep(1.0)

    # The per-frame refresh must have picked up pkghello: it should be
    # the sole card in the Packages category right now, no TAB needed.
    print("[6] screendump Packages category")
    if screendump(PPM):
        if has_magenta_icon(PPM):
            print("    MAGENTA ICON FOUND — refreshed without reopening!")
            ok = True
        else:
            print("    no magenta icon — launcher list is stale")
    else:
        print("    screendump failed")

finally:
    if qemu.poll() is None:
        qemu.stdin.write("quit\n".encode())
        qemu.stdin.flush()
        qemu.wait(timeout=10)
    store.terminate()

print("\n" + ("PASS: launcher refreshed after install without reopening"
              if ok else "FAIL: launcher list did not refresh"))
sys.exit(0 if ok else 1)
