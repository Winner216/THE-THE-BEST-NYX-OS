#!/usr/bin/env python3
"""_wm_e2e_test.py — native window manager behavior in Nyx OS.

Boots the ISO, logs in, and verifies the WM layer end to end:

  1. F1 opens Terminal → taskbar tray shows ONE window button
     (accent-green dot, 0x2ECC71, in the middle tray band).
  2. Clicking the window's [X] (top-right of the focused window)
     closes it → tray empties, back on the desktop.
  3. F1 (Terminal) + F2 (Files) → tray shows TWO buttons
     (green Terminal + amber Files 0xF2B73D).
  4. Alt+Tab switches windows (the screen's window area changes).
  5. [X] closes the focused window (green gone, amber remains);
     [X] again closes the last one (tray empty).

QEMU monitor mouse: `mouse_move dx dy` (relative), `mouse_button 1/0`.
The cursor starts at screen center; the driver is 1:1 only for ±1px
deltas, so long moves are sent one pixel at a time (see _qs_panel_test).
"""
import os
import subprocess
import sys
import time

WORK = "/tmp/wmtest"
os.makedirs(WORK, exist_ok=True)
SERIAL = os.path.join(WORK, "serial.log")
try:
    open(SERIAL, "w").close()
except Exception:
    pass

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


def keys(seq, gap=0.18):
    for k in seq:
        send("sendkey " + k, gap)


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


def move_mouse(dx, dy):
    if dx > 0:
        for _ in range(dx):
            send("mouse_move 1 0", 0.012)
    else:
        for _ in range(-dx):
            send("mouse_move -1 0", 0.012)
    if dy > 0:
        for _ in range(dy):
            send("mouse_move 0 1", 0.012)
    else:
        for _ in range(-dy):
            send("mouse_move 0 -1", 0.012)


def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = data[3 + len(parts[1]) + 1 + len(parts[2]) + 1:]
    return w, h, px


def count_color(px, w, h, x0, y0, x1, y1, rgb):
    r, g, b = rgb
    n = 0
    for y in range(y0, min(y1, h)):
        row = y * w * 3
        for x in range(x0, min(x1, w)):
            i = row + x * 3
            if px[i] == r and px[i + 1] == g and px[i + 2] == b:
                n += 1
    return n


GREEN = (0x2E, 0xCC, 0x71)   # Terminal accent 0x2ECC71
AMBER = (0xF2, 0xB7, 0x3D)   # Files accent    0xF2B73D

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

    # Tray band: TOP 36px, middle (draw_taskbar renders at rows
    # 0..36). Window [X]: Terminal/Files both open at g_win=(192,70),
    # w=W*8/10=1536 → [X] ≈ (1708, 86).

    def tray_counts(shot):
        sw, sh, spx = load_ppm(shot)
        x0, y0, x1, y1 = sw // 2 - 60, 0, sw // 2 + 300, 36
        g = count_color(spx, sw, sh, x0, y0, x1, y1, GREEN)
        a = count_color(spx, sw, sh, x0, y0, x1, y1, AMBER)
        return g, a

    # ── 3. Open Terminal only → 1 tray button ────────────────────
    print("[3] F1 → Terminal")
    send("sendkey f1", 1.5)
    send("screendump " + WORK + "/t1_term.ppm", 1.0)
    g, a = tray_counts(WORK + "/t1_term.ppm")
    print(f"    tray: green={g} amber={a}")
    if g < 10:
        print("FAIL: Terminal tray button not rendered"); ok = False
    else:
        print("PASS: Terminal window in tray")

    # ── 4. Click [X] → window closes, tray empties ───────────────
    print("[4] clicking [X] at (1708,86)...")
    move_mouse(1708 - 960, 86 - 540)       # center → close button
    send("mouse_button 1", 0.4)
    send("mouse_button 0", 0.4)
    time.sleep(1.2)
    send("screendump " + WORK + "/t2_closed.ppm", 1.0)
    g, a = tray_counts(WORK + "/t2_closed.ppm")
    print(f"    tray: green={g} amber={a}")
    if g >= 10:
        print("FAIL: Terminal still in tray after [X]"); ok = False
    else:
        print("PASS: [X] closed the Terminal window")

    # ── 5. Open Terminal + Files → 2 tray buttons ────────────────
    print("[5] F1 Terminal, F2 Files")
    send("sendkey f1", 1.5)
    send("sendkey f2", 1.5)
    send("screendump " + WORK + "/t3_two.ppm", 1.0)
    g, a = tray_counts(WORK + "/t3_two.ppm")
    print(f"    tray: green={g} amber={a}")
    if g < 10 or a < 10:
        print("FAIL: expected 2 tray buttons (green+amber)"); ok = False
    else:
        print("PASS: both windows in tray")

    # ── 6. Alt+Tab switches windows (window area changes) ────────
    print("[6] Alt+Tab (600ms hold)...")
    send("sendkey alt-tab 600", 1.5)
    time.sleep(1.0)
    send("screendump " + WORK + "/t4_switched.ppm", 1.0)
    w1, h1, px1 = load_ppm(WORK + "/t3_two.ppm")
    w2, h2, px2 = load_ppm(WORK + "/t4_switched.ppm")
    # Count differing pixels in the window area (excluding taskbar).
    diff = 0
    for y in range(80, min(h1, h2) - 40, 4):
        for x in range(200, min(w1, w2) - 60, 4):
            i1 = (y * w1 + x) * 3
            i2 = (y * w2 + x) * 3
            if (px1[i1] != px2[i2] or px1[i1 + 1] != px2[i2 + 1]
                    or px1[i1 + 2] != px2[i2 + 2]):
                diff += 1
    print(f"    window-area diff px={diff}")
    if diff < 2000:
        print("FAIL: Alt+Tab did not visibly switch windows"); ok = False
    else:
        print("PASS: Alt+Tab switched the focused window")

    # ── 7. [X] closes focused (Terminal), Files remains ──────────
    print("[7] clicking [X] → closes focused window...")
    send("mouse_button 1", 0.4)
    send("mouse_button 0", 0.4)
    time.sleep(1.2)
    send("screendump " + WORK + "/t5_oneleft.ppm", 1.0)
    g, a = tray_counts(WORK + "/t5_oneleft.ppm")
    print(f"    tray: green={g} amber={a}")
    if g >= 10 or a < 10:
        print("FAIL: expected only Files left in tray"); ok = False
    else:
        print("PASS: focused window closed, Files remains")

    # ── 8. [X] again closes the last window → desktop, empty tray ─
    print("[8] clicking [X] again...")
    send("mouse_button 1", 0.4)
    send("mouse_button 0", 0.4)
    time.sleep(1.2)
    send("screendump " + WORK + "/t6_desktop.ppm", 1.0)
    g, a = tray_counts(WORK + "/t6_desktop.ppm")
    print(f"    tray: green={g} amber={a}")
    if g >= 10 or a >= 10:
        print("FAIL: tray not empty after closing all windows"); ok = False
    else:
        print("PASS: all windows closed, back on desktop")

finally:
    qemu.stdin.write("quit\n".encode())
    qemu.stdin.flush()
    qemu.wait(timeout=10)

print("\n" + ("PASS: window manager verified" if ok else "FAIL: WM checks failed"))
sys.exit(0 if ok else 1)
