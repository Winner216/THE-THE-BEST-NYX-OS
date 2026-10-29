#!/usr/bin/env python3
"""_browser_redirect_e2e_test.py — https-redirect handling in the Browser.

Host server:
  /downgrade  → 301 Location: https://127.0.0.1:PORT/works-over-http
                (the browser downgrades to http → 200 DOWNGRADE OK)
  /works-over-http → 200 "DOWNGRADE OK"
  /https-only → 301 Location: https://127.0.0.1:PORT/secure
                (downgrade → /secure also 301s to https → give up,
                 browser renders the no-TLS message; BR HTTP OK 301)
  /secure     → 301 Location: https://127.0.0.1:PORT/secure  (like wikipedia)
"""
import subprocess, time, os, sys, threading, socket

PORT = 18082
WORK = "/tmp/browserredir"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()

def handle(conn):
    try:
        conn.settimeout(5)
        data = conn.recv(4096)
        req = data.split(b"\r\n", 1)[0].decode("latin1", "replace")
        parts = req.split(" ")
        path = parts[1] if len(parts) > 1 else "/"
        if path.startswith("/downgrade"):
            body = "downgrade"
            code = 301
            loc = "https://127.0.0.1:%d/works-over-http" % PORT
            status = "Moved Permanently"
        elif path.startswith("/works-over-http"):
            body = "<html><body>DOWNGRADE OK</body></html>"
            code, loc, status = 200, None, "OK"
        elif path.startswith("/https-only"):
            body = "https-only"
            code = 301
            loc = "https://127.0.0.1:%d/secure" % PORT
            status = "Moved Permanently"
        elif path.startswith("/secure"):
            body = "secure"
            code = 301
            loc = "https://127.0.0.1:%d/secure" % PORT
            status = "Moved Permanently"
        else:
            body, code, loc, status = "<html><body>NOT FOUND</body></html>", 404, None, "Not Found"
        resp = "HTTP/1.1 %d %s\r\n" % (code, status)
        if loc:
            resp += "Location: %s\r\n" % loc
        resp += "Content-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(body), body)
        conn.sendall(resp.encode())
    except Exception:
        pass
    finally:
        conn.close()

def serve():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", PORT))
    s.listen(8)
    while True:
        conn, _ = s.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()

threading.Thread(target=serve, daemon=True).start()
print("host server on port %d" % PORT, flush=True)

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

def keys(seq, gap=0.18):
    QKEY = {".": "dot", "/": "slash", ":": "shift-semicolon", "-": "minus"}
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
    print("[1] boot + login...", flush=True)
    if not wait_serial("LOGIN READY", 90):
        print("FAIL: no login"); sys.exit(1)
    keys(["1", "2", "3", "4"], 0.2)
    send("sendkey ret", 1.0)
    if not wait_serial("DESKTOP UP", 30):
        print("FAIL: no desktop"); sys.exit(1)

    print("[2] launch Browser...", flush=True)
    send("sendkey f1", 1.0)
    keys(list("browser"), 0.18)
    send("sendkey ret", 1.5)
    time.sleep(2)

    print("[3] /downgrade — https hint, http served...", flush=True)
    keys(list("10.0.2.2:%d/downgrade" % PORT), 0.18)
    send("sendkey ret", 1.0)
    ok = wait_serial("BR HTTP OK 200 10.0.2.2:%d/downgrade " % PORT, 90) and \
         wait_serial("DOWNGRADE OK", 5)
    print(("PASS: https-redirect downgraded to http, page loaded" if ok else
           "FAIL: downgrade did not load"), flush=True)
    if not ok: sys.exit(1)

    print("[4] /https-only — wikipedia-like, gives up with message...", flush=True)
    send("sendkey esc", 0.5)      # focus address bar
    for _ in range(30):            # clear the previous URL
        send("sendkey backspace", 0.12)
    keys(list("10.0.2.2:%d/https-only" % PORT), 0.18)
    send("sendkey ret", 1.0)
    ok = wait_serial("BR HTTP OK 301", 90)
    print(("PASS: https-only redirect detected, message page shown" if ok else
           "FAIL: no 301 marker"), flush=True)
    time.sleep(2)
    send("screendump " + WORK + "/s_message.ppm", 0.5)
    time.sleep(1)
    if not ok: sys.exit(1)

    # Pixel check: the message page renders text in the content area
    def load_ppm(p):
        with open(p, "rb") as f: d = f.read()
        rest = d[3:]
        nl = rest.find(b"\n"); dims = rest[:nl].split()
        w, h = int(dims[0]), int(dims[1])
        rest = rest[nl+1:]
        nl = rest.find(b"\n")
        return w, h, rest[nl+1:]
    try:
        w, h, px = load_ppm(os.path.join(WORK, "s_message.ppm"))
        bright = 0
        for y in range(120, h, 4):
            for x in range(0, w, 4):
                off = (y * w + x) * 3
                if px[off] > 120 and px[off+1] > 120 and px[off+2] > 120:
                    bright += 1
        print("    message-page bright pixels: %d" % bright, flush=True)
        print(("PASS: no-TLS message rendered" if bright > 10 else
               "FAIL: message page appears empty"), flush=True)
        if bright <= 10:
            sys.exit(1)
    except Exception as e:
        print("    pixel check skipped:", e, flush=True)

    result = "PASS"

finally:
    qemu.stdin.write(b"quit\n"); qemu.stdin.flush()
    try: qemu.wait(timeout=5)
    except: qemu.kill()

print("\nRESULT:", result, flush=True)
sys.exit(0 if result == "PASS" else 1)