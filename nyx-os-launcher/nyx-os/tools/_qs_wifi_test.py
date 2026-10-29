#!/usr/bin/env python3
"""E2E test for the Quick Settings Wi-Fi flyout.

Flow: boot → login → desktop → open Quick Settings (double-click icon)
→ click the Wi-Fi chevron → flyout opens (scanning, then 6 network
rows) → click row 2 ("FiberHome-5G") → connected state moves there →
click outside the flyout (but inside the panel) → flyout closes, panel
stays → verify via screenshot pixel analysis.
"""
import subprocess, sys, time, os, shutil

WORK = "/tmp/qswifitest"
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK, exist_ok=True)
SERIAL = WORK + "/serial.log"

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

def keys(seq, gap=0.2):
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

def click():
    send("mouse_button 1", 0.15)
    send("mouse_button 0", 0.15)

def shot(name):
    send("screendump " + WORK + "/" + name + ".ppm", 1.0)

def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    return w, h, px

def count_in(px, w, h, x0, y0, x1, y1, pred):
    n = 0
    for y in range(y0, min(y1, h)):
        row = y * w * 3
        for x in range(x0, min(x1, w)):
            i = row + x * 3
            if pred(px[i], px[i + 1], px[i + 2]):
                n += 1
    return n

ok = True
try:
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: no login"); ok = False; sys.exit(1)
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: no desktop"); ok = False; sys.exit(1)
    print("[1] desktop up")

    cx, cy = 960, 540

    # Open Quick Settings (double-click icon at ~616,195)
    move_mouse(616 - cx, 195 - cy); cx, cy = 616, 195
    click(); click()
    time.sleep(1.5)

    # Click the Wi-Fi chevron (grid slot 0 right edge: ~941,517)
    move_mouse(941 - cx, 517 - cy); cx, cy = 941, 517
    click()
    time.sleep(0.3)
    shot("scanning")
    time.sleep(2.0)
    shot("netlist")
    time.sleep(0.3)

    w, h, px = load_ppm(WORK + "/netlist.ppm")
    fx, fy, fw, fh = 1148, 325, 330, 410

    def is_orange(r, g, b):
        return r > 0xE0 and 0x50 < g < 0x80 and b < 0x60
    def is_white(r, g, b):
        return r > 0xE0 and g > 0xE0 and b > 0xE0
    def is_panel(r, g, b):
        return abs(r - 0x1E) < 10 and abs(g - 0x1E) < 10 and abs(b - 0x1E) < 10

    # 1. Flyout surface present (right of the main panel)
    panel = count_in(px, w, h, fx + 30, fy + 30, fx + fw - 30, fy + fh - 30, is_panel)
    if panel > 30000:
        print("[2] PASS: flyout surface rendered")
    else:
        print(f"[2] FAIL: flyout surface (panel px={panel})"); ok = False

    # 2. Toggle is ON (orange fill + white knob on the right)
    togg = count_in(px, w, h, fx + fw - 62, fy + 13, fx + fw - 18, fy + 35, is_orange)
    knob = count_in(px, w, h, fx + fw - 40, fy + 16, fx + fw - 14, fy + 30, is_white)
    if togg > 200 and knob > 20:
        print("[3] PASS: toggle on (orange fill + right knob)")
    else:
        print(f"[3] FAIL: toggle (orange={togg} knob={knob})"); ok = False

    # 3. All 6 network rows render (non-panel pixels in each row band)
    for i in range(6):
        ry = fy + 58 + i * 50
        n = count_in(px, w, h, fx + 12, ry + 2, fx + fw - 12, ry + 44,
                     lambda r, g, b: not is_panel(r, g, b))
        if n < 60:
            print(f"[4] FAIL: row {i} has only {n} content px"); ok = False
    if ok:
        print("[4] PASS: all 6 network rows rendered")

    # 4. Row 0 is connected (orange accents), rows 1-5 not
    r0 = count_in(px, w, h, fx + 12, fy + 58, fx + fw - 12, fy + 58 + 46, is_orange)
    r1 = count_in(px, w, h, fx + 12, fy + 108, fx + fw - 12, fy + 108 + 46, is_orange)
    if r0 > 100 and r1 < 20:
        print("[5] PASS: row 0 connected, others not")
    else:
        print(f"[5] FAIL: row0 orange={r0} row1 orange={r1}"); ok = False

    # 5. Click row 1 ("FiberHome-5G" at ~1310,456) → connected moves
    move_mouse(1310 - cx, 456 - cy); cx, cy = 1310, 456
    click()
    time.sleep(1.0)
    shot("selected")
    w, h, px = load_ppm(WORK + "/selected.ppm")
    r0 = count_in(px, w, h, fx + 12, fy + 58, fx + fw - 12, fy + 58 + 46, is_orange)
    r1 = count_in(px, w, h, fx + 12, fy + 108, fx + fw - 12, fy + 108 + 46, is_orange)
    if r1 > 100 and r0 < 20:
        print("[6] PASS: connected state moved to row 1")
    else:
        print(f"[6] FAIL: row0 orange={r0} row1 orange={r1}"); ok = False

    # 6. Click outside the flyout but inside the panel (volume slider area)
    move_mouse(800 - cx, 420 - cy); cx, cy = 800, 420
    click()
    time.sleep(1.0)
    shot("after_close")
    time.sleep(3.0)
    shot("after_close2")
    w, h, px = load_ppm(WORK + "/after_close2.ppm")
    # Flyout gone → no orange accents (toggle/connected row) in its region
    fly_org = count_in(px, w, h, fx + 20, fy + 20, fx + fw - 20, fy + fh - 20, is_orange)
    if fly_org < 20:
        print("[7] PASS: flyout closed on outside click")
    else:
        print(f"[7] FAIL: flyout still open (orange px={fly_org})"); ok = False
    # Main panel still open → volume slider orange fill present
    main_org = count_in(px, w, h, 800, 410, 1150, 428, is_orange)
    if main_org > 100:
        print("[8] PASS: main panel still open (slider visible)")
    else:
        print(f"[8] FAIL: main panel gone (slider orange px={main_org})"); ok = False

finally:
    try:
        send("quit")
    except Exception:
        pass
    qemu.wait(timeout=10)

print()
print("PASS: quick settings Wi-Fi flyout verified" if ok else "FAIL: see above")
sys.exit(0 if ok else 1)
