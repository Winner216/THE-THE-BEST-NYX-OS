#!/usr/bin/env python3
"""Boot test for the Windows-11-style Quick Settings panel.

Boots the Nyx ISO in QEMU (KVM), logs in, moves the PS/2 cursor to the
QuickSettings desktop icon (grid slot 18) via single-pixel monitor
moves, clicks it, and screendumps. Pixel analysis asserts the panel
rendered: #1E1E1E surface, #FF5722 orange accents (slider fills +
active Wi-Fi/Dark Style buttons), #2C2C2C element circles, and white
slider thumbs. Then clicks outside the panel and asserts it closed.

QEMU monitor mouse: `mouse_move dx dy` (relative), `mouse_button 1/0`.
The cursor starts at screen center; the driver is 1:1 only for ±1px
deltas, so long moves are sent one pixel at a time.
"""
import os
import subprocess
import sys
import time

WORK = "/tmp/qstest"
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
    # single-pixel steps keep the 1:1 precision zone
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
    # P6 header: P6\n W H\n 255\n
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


ok = True
try:
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: no login screen"); sys.exit(1)
    print("[1] login screen up")
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up"); sys.exit(1)
    print("[2] desktop up (1920x1080 assumed)")

    # Cursor starts at center (960,540). QuickSettings icon = grid slot 18
    # (col 4, row 1) → tile center ≈ (616, 195).
    print("[3] moving cursor to QuickSettings icon...")
    move_mouse(616 - 960, 195 - 540)
    send("screendump " + WORK + "/s1_hover.ppm", 1.0)

    print("[4] double-clicking (icons launch on double-click)...")
    for _ in range(2):
        send("mouse_button 1", 0.15)
        send("mouse_button 0", 0.15)
    time.sleep(2.0)
    send("screendump " + WORK + "/s2_panel.ppm", 1.0)

    w, h, px = load_ppm(WORK + "/s2_panel.ppm")
    print(f"    shot size {w}x{h}")
    # Panel is centered: 400x410 at (W-400)/2, (H-410)/2-10
    px0 = (w - 400) // 2
    py0 = (h - 410) // 2 - 10
    cx0, cy0 = px0 + 20, py0 + 20
    cx1, cy1 = px0 + 380, py0 + 390

    panel = count_color(px, w, h, cx0, cy0, cx1, cy1, (0x1E, 0x1E, 0x1E))
    orange = count_color(px, w, h, cx0, cy0, cx1, cy1, (0xFF, 0x57, 0x22))
    elem = count_color(px, w, h, cx0, cy0, cx1, cy1, (0x2C, 0x2C, 0x2C))
    white = count_color(px, w, h, cx0, cy0, cx1, cy1, (0xFF, 0xFF, 0xFF))

    print(f"    panel px={panel} orange px={orange} elem px={elem} white px={white}")
    if panel < 20000:
        print("FAIL: panel surface (#1E1E1E) not detected"); ok = False
    else:
        print("PASS: panel surface rendered")
    if orange < 200:
        print("FAIL: orange accent (#FF5722) not detected"); ok = False
    else:
        print("PASS: orange accents (slider fills / active buttons)")
    if elem < 1000:
        print("FAIL: #2C2C2C element surfaces not detected"); ok = False
    else:
        print("PASS: element surfaces (pill, circles, buttons)")
    if white < 100:
        print("FAIL: white slider thumbs / text not detected"); ok = False
    else:
        print("PASS: white thumbs/text")

    # ── Click outside the panel → should close it (flyout behavior) ──
    print("[5] clicking outside panel...")
    move_mouse(0, -300)          # up and away from the panel
    send("mouse_button 1", 0.4)
    send("mouse_button 0", 0.4)
    time.sleep(1.5)
    send("screendump " + WORK + "/s3_closed.ppm", 1.0)
    w2, h2, px2 = load_ppm(WORK + "/s3_closed.ppm")
    panel2 = count_color(px2, w2, h2, cx0, cy0, cx1, cy1, (0x1E, 0x1E, 0x1E))
    if panel2 > 5000:
        print("FAIL: panel still open after outside click"); ok = False
    else:
        print("PASS: outside click closed the panel")

finally:
    qemu.stdin.write("quit\n".encode())
    qemu.stdin.flush()
    qemu.wait(timeout=10)

print("\n" + ("PASS: quick settings panel verified" if ok else "FAIL: panel checks failed"))
sys.exit(0 if ok else 1)
