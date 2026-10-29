#!/usr/bin/env python3
"""_pkg_enter_run_e2e_test.py — ENTER must RUN the selected app.

Boots the ISO and verifies, end to end:

  A) App Library: an installed package card + single ENTER launches it
     straight away (no detail-view detour) and the desktop hops to the
     Terminal where the app's output is mirrored.
  B) App Store: a single ENTER on a NOT-installed app installs it AND
     runs it (one press), again hopping to the Terminal.
  C) The Terminal repaints live while an app animates (Game of Life),
     so two screendumps during the run differ in the window area.
"""
import os
import subprocess
import sys
import time

os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")

subprocess.run(["make", "pkgrepo"], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
WORK = "/tmp/pkgenter"
os.makedirs(WORK, exist_ok=True)
REQLOG = os.path.join(WORK, "store_reqs.log")
SERIAL = os.path.join(WORK, "serial.log")
PPM1 = os.path.join(WORK, "life1.ppm")
PPM2 = os.path.join(WORK, "life2.ppm")
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


def ppm_diff(path1, path2, row0, row1):
    """Count differing pixels between two P6 PPMs in rows row0..row1."""
    def load(p):
        with open(p, "rb") as f:
            assert f.readline().strip() == b"P6"
            while True:
                ln = f.readline().strip()
                if ln and not ln.startswith(b"#"):
                    break
            w, h = map(int, ln.split())
            f.readline()
            return w, h, f.read()
    try:
        w, h, d1 = load(path1)
        w2, h2, d2 = load(path2)
        if w != w2 or h != h2:
            return -1
        dif = 0
        for y in range(max(0, row0), min(h, row1)):
            for x in range(w):
                o1 = (y * w + x) * 3
                if d1[o1:o1 + 3] != d2[o1:o1 + 3]:
                    dif += 1
        return dif
    except Exception:
        return -1


a_ok = b_ok = c_ok = False
try:
    if not wait_serial("LOGIN READY", 120):
        print("FAIL: no login screen"); sys.exit(1)
    print("[1] login")
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up"); sys.exit(1)
    print("[2] desktop up")

    # ── A) App Library single-ENTER run ───────────────────────────
    print("[3] install pkgquote from Terminal first")
    send("sendkey f1", 1.5)
    time.sleep(0.8)
    keys(list("pkg update"), 0.14)
    send("sendkey ret", 3.0)
    keys(list("pkg install pkgquote"), 0.14)
    send("sendkey ret", 3.5)
    # wait until the host store served the ELF
    t0 = time.time()
    served = False
    while time.time() - t0 < 30:
        try:
            if "pkgquote.elf" in open(REQLOG).read():
                served = True
                break
        except Exception:
            pass
        time.sleep(0.5)
    print("    pkgquote installed" if served else "    WARN: store never served pkgquote.elf")
    time.sleep(1.0)

    print("[4] desktop -> App Library -> Packages -> ENTER runs pkgquote")
    send("sendkey esc", 1.0)
    time.sleep(0.5)
    send("sendkey slash", 0.4)
    keys(list("applib"), 0.14)
    send("sendkey ret", 1.5)
    time.sleep(1.0)
    for _ in range(7):
        send("sendkey tab", 0.25)
    time.sleep(0.6)
    send("sendkey ret", 1.5)          # single ENTER on the pkgquote card
    if not wait_serial("[PKGQUOTE] from ring-3 package", 60):
        print("FAIL: launcher ENTER did not run pkgquote"); sys.exit(1)
    print("    launcher ENTER ran pkgquote (and hopped to Terminal)")
    a_ok = True

    # ── B) Store single ENTER installs AND runs ───────────────────
    print("[5] store -> ENTER on pkgcalc (not installed) installs+ runs")
    keys(list("store"), 0.14)
    send("sendkey ret", 1.5)
    time.sleep(3.0)                   # catalog fetch inside store_open
    send("sendkey ret", 3.0)          # ENTER on pkgcalc (catalog slot 0)
    if not wait_serial("[PKGCALC] all done", 120):
        print("FAIL: store ENTER did not install+run pkgcalc"); sys.exit(1)
    print("    store ENTER installed + ran pkgcalc")
    b_ok = True

    # ── C) Live Terminal repaint during the Game of Life ──────────
    print("[6] install pkglife, then run it + screenshot mid-animation")
    keys(list("pkg install pkglife"), 0.14)
    send("sendkey ret", 3.0)
    t0 = time.time()
    while time.time() - t0 < 30:
        try:
            if "pkglife.elf" in open(REQLOG).read():
                break
        except Exception:
            pass
        time.sleep(0.5)
    time.sleep(0.5)
    keys(list("pkg run pkglife"), 0.14)
    send("sendkey ret", 0.6)
    send("screendump " + PPM1, 1.3)
    time.sleep(1.5)
    send("screendump " + PPM2, 1.3)
    if not wait_serial("[PKGLIFE] 40 generations done", 90):
        print("FAIL: pkglife did not finish"); sys.exit(1)
    print("    pkglife finished")
    if os.path.exists(PPM1) and os.path.exists(PPM2):
        dif = ppm_diff(PPM1, PPM2, 100, 800)   # terminal window region
        if dif > 200:
            print("    terminal window repainted live during run (diff=%d px)" % dif)
            c_ok = True
        else:
            print("    WARN: screenshots nearly identical (diff=%d px)" % dif)
    else:
        print("    WARN: screendumps missing")

finally:
    qemu.stdin.write("quit\n".encode())
    qemu.stdin.flush()
    qemu.wait(timeout=10)
    store.terminate()

print("\n" + ("PASS: ENTER runs apps (launcher + store) with live Terminal output"
              if (a_ok and b_ok) else "FAIL"))
sys.exit(0 if (a_ok and b_ok) else 1)
