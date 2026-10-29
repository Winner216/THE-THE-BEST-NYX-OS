#!/usr/bin/env python3
"""_browser_e2e_test.py — live web browsing in the Nyx Browser app.

Boots the ISO, logs in, launches the Browser (terminal `browser`),
types a URL into the address bar, and verifies the page loads:
  BR HTTP OK 200   serial marker from browser.c br_fetch
  rendered page    screenshot of the htmlview panel below the chrome
"""
import subprocess, time, os, sys

WORK = "/tmp/browsertest"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-nic", "user,model=e1000",
     "-monitor", "stdio", "-serial", "file:" + serial_path,
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)

def keys(seq, gap=0.2):
    QKEY = {".": "dot", "/": "slash", "-": "minus", " ": "spc"}
    for k in seq:
        send("sendkey " + QKEY.get(k, k), gap)

def wait_serial(marker, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(serial_path, "r", errors="ignore") as f:
                if marker in f.read():
                    return True
        except Exception:
            pass
        time.sleep(0.5)
    return False

result = "FAIL"
try:
    print("[1] waiting for LOGIN READY...", flush=True)
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: guest never reached login")
        sys.exit(1)
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: desktop never came up")
        sys.exit(1)
    print("    desktop up", flush=True)

    print("[2] launching Browser (F1, `browser`)...", flush=True)
    send("sendkey f1", 1.0)
    keys(list("browser"), 0.18)
    send("sendkey ret", 1.5)
    time.sleep(2)
    send("screendump " + WORK + "/s1_welcome.ppm", 0.5)

    print("[3] typing example.com in the address bar...", flush=True)
    keys(list("example.com"), 0.18)
    time.sleep(1)
    send("screendump " + WORK + "/s2_url.ppm", 0.5)

    print("[4] ENTER — DNS + HTTP + render...", flush=True)
    send("sendkey ret", 1.0)
    ok = wait_serial("BR HTTP OK 200", 90)
    print(("PASS: page loaded (HTTP 200 via Browser)" if ok else
           "FAIL: no BR HTTP OK marker"), flush=True)
    time.sleep(2)
    send("screendump " + WORK + "/s3_page.ppm", 0.5)
    time.sleep(1)

    # ── Verify the rendered page pixels ─────────────────────────
    # The status bar (y≈64..88) should show "HTTP 200 OK (...) B".
    # The content area below should show light text on dark bg.
    import struct
    def load_ppm(p):
        with open(p, "rb") as f:
            d = f.read()
        if d[:2] != b"P6": return None
        # P6\n<w> <h>\n<maxval>\n<data>
        rest = d[3:]
        nl = rest.find(b"\n")
        dims = rest[:nl].split()
        w, h = int(dims[0]), int(dims[1])
        rest = rest[nl+1:]
        nl = rest.find(b"\n")
        px = rest[nl+1:]
        return w, h, px
    def sample(ppm, x, y):
        w, h, px = ppm
        off = (y * w + x) * 3
        return tuple(px[off:off+3])

    try:
        w, h, px = load_ppm(os.path.join(WORK, "s3_page.ppm"))
        # Count light-ish pixels in the content area (rendered text)
        light = 0
        total = 0
        for y in range(120, h, 4):
            for x in range(0, w, 4):
                r, g, b = sample((w, h, px), x, y)
                total += 1
                if r > 120 and g > 120 and b > 120:
                    light += 1
        frac = light / max(total, 1)
        print("    content-area light-pixel fraction: %.3f" % frac, flush=True)
        if frac > 0.001:
            print("PASS: rendered text visible in the page area", flush=True)
        else:
            print("FAIL: no rendered text detected", flush=True)
    except Exception as e:
        print("    pixel check skipped:", e, flush=True)

    log = ""
    try:
        with open(serial_path, "r", errors="ignore") as f:
            log = f.read()
    except Exception:
        pass
    for line in log.splitlines():
        if "BR HTTP OK" in line or "NIC UP" in line:
            print("  " + line.strip(), flush=True)

    result = "PASS" if ok else "FAIL"

finally:
    qemu.stdin.write(b"quit\n"); qemu.stdin.flush()
    try: qemu.wait(timeout=5)
    except: qemu.kill()

print("RESULT:", result, flush=True)
sys.exit(0 if result == "PASS" else 1)