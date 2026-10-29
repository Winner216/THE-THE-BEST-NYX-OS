#!/usr/bin/env python3
"""_chess_mouse_probe.py — calibrate QMP mouse pacing for CHESS.EXE
using the game's TEMP [DBG] serial telemetry (cursor pos + raw counts),
not pixel heuristics.
"""
import subprocess, time, os, sys, socket, json, re

WORK = "/tmp/chessmouse"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()
QMP_SOCK = os.path.join(WORK, "qmp.sock")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-audiodev", "none,id=snd", "-device", "AC97,audiodev=snd",
     "-monitor", "stdio", "-serial", "file:" + serial_path,
     "-display", "none",
     "-qmp", "unix:" + QMP_SOCK + ",server,nowait"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

def readlog():
    try: return open(serial_path, "rb").read().decode("latin1", "replace")
    except Exception: return ""

def wait_log(sub, timeout=240):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sub in readlog(): return True
        time.sleep(1)
    return False

def qmp_connect(path, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = None
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            f = s.makefile("rw")
            json.loads(f.readline())
            f.write('{"execute":"qmp_capabilities"}\n'); f.flush()
            json.loads(f.readline())
            return s, f
        except Exception:
            if s: s.close()
            time.sleep(0.5)
    raise RuntimeError("QMP connect failed")

qmp, qmpf = qmp_connect(QMP_SOCK)

def qmp_send(events):
    qmpf.write(json.dumps({"execute": "input-send-event",
                           "arguments": {"events": events}}) + "\n")
    qmpf.flush()
    try: json.loads(qmpf.readline())
    except Exception: pass

def rel(dx=0, dy=0):
    evs = []
    if dx: evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
    if dy: evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
    if evs: qmp_send(evs)

def sel_lines():
    return [l for l in readlog().splitlines() if l.startswith("SEL ")]

def step(key):
    before = len(sel_lines())
    send("sendkey " + key, 0.2)
    dl = time.time() + 4
    while time.time() < dl:
        if len(sel_lines()) > before: return True
        time.sleep(0.15)
    return False

def nav_to_name(target_name):
    guard = 0
    if not step("shift-b"):
        return False
    while guard < 90:
        s = sel_lines()
        if s:
            parts = s[-1].split(None, 2)
            seen = parts[2] if len(parts) >= 3 else ""
            if seen == target_name: return True
            if seen < target_name:
                if not step("shift-b"): return False
            else:
                if not step("shift-a"): return False
        guard += 1
    return False

def last_dbg():
    lines = [l for l in readlog().splitlines() if l.startswith("[DBG]")]
    if not lines: return None
    f = lines[-1].split()[1:]
    def s32(h): v = int(h, 16); return v - (1 << 32) if v >= (1 << 31) else v
    return tuple(s32(x) for x in f)   # mx, my, accx, accy, rawx, rawy

def measure(label, n, pace, chunk):
    """Send n unit-chunks on X, settle, read DBG; then Y.  No slams:
    QMP rel events queue and drain at a finite rate, so every phase is
    followed by a long settle and we read the game's own telemetry
    (position + raw counts) instead of pixels."""
    time.sleep(3.0)                                  # drain queue
    a = last_dbg()
    for _ in range(0, n, chunk):
        rel(dx=chunk)
        time.sleep(pace)
    time.sleep(3.0)                                  # settle
    b = last_dbg()
    c = last_dbg()                                  # base for Y
    for _ in range(0, n, chunk):
        rel(dy=chunk)
        time.sleep(pace)
    time.sleep(3.0)
    d = last_dbg()                                  # noqa: F841 kept for parity
    print(f"RESULT {label:14s} dx={b[0]-a[0]:5d}/exp{n//3:<4d} "
          f"rawX={b[4]-a[4]:6d}/exp{n:<6d} | "
          f"dy={d[1]-c[1]:5d}/exp{n//3:<4d} "
          f"rawY={d[5]-c[5]:6d}/exp{n:<6d}", flush=True)

SCHEMES = [
    ("paced900@1ms",   900, 0.001, 1),
    ("paced900@4ms",   900, 0.004, 1),
    ("chunk3x300@25ms", 900, 0.025, 3),
]

try:
    print("booting...", flush=True)
    assert wait_log("LOGIN READY", 120), "no LOGIN READY"
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    assert wait_log("DESKTOP UP", 120), "no DESKTOP UP"
    time.sleep(5)
    send("sendkey f2", 1.5); time.sleep(1)
    for _ in range(5): step("shift-b")
    send("sendkey ret", 1.0); time.sleep(1.5)
    assert nav_to_name("CHESS.EXE"), "nav failed"
    send("sendkey ret", 1.5)
    assert wait_log("PE: loading CHESS.EXE", 60), "no PE load"
    assert wait_log("[CHESS] up", 60), "no [CHESS] up"
    time.sleep(3)
    for s in SCHEMES:
        try: measure(*s)
        except Exception as e: print("RESULT", s[0], "ERR", e, flush=True)
    print("final:", last_dbg(), flush=True)
finally:
    send("quit", 0.4)
    try: qemu.wait(timeout=10)
    except Exception: pass
    try: qemu.kill()
    except Exception: pass
