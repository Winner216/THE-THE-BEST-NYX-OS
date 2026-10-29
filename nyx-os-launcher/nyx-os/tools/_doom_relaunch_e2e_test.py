#!/usr/bin/env python3
"""E2E: DOOM relaunch — start game, close via [X], press Enter again.

Boot → login → Files → Local Disk → DOOM.EXE → Enter (launch #1)
→ demo runs → click the [X] box (close, back to Files)
→ press Enter again (launch #2, same selection)
→ assert: a SECOND "PE: loading" appears and the game reaches
  "[DOOM] doomgeneric_Create" again, with no kernel fault.
"""
import subprocess, time, os, shutil, re, socket, json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
WORK = "/tmp/doomrelaunch"
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK, exist_ok=True)
SERIAL = WORK + "/serial.log"
QMP_SOCK = os.path.join(WORK, "qmp.sock")

def readlog():
    try:
        with open(SERIAL, "r", errors="ignore") as f:
            return f.read()
    except Exception:
        return ""

def count(sub):
    return readlog().count(sub)

def wait_log(sub, timeout=120):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sub in readlog(): return True
        time.sleep(1)
    return False

def wait_count(sub, n, timeout=120):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if count(sub) >= n: return True
        time.sleep(1)
    return False

def sel_lines():
    return [l for l in readlog().splitlines() if l.startswith("SEL ")]

def last_row():
    s = sel_lines()
    if not s: return None
    parts = s[-1].split()
    return int(parts[1]) if len(parts) >= 2 else None

qemu = subprocess.Popen(
    ["qemu-system-i386", "-enable-kvm", "-cpu", "host",
     "-cdrom", "nyx.iso", "-m", "1G", "-boot", "d",
     "-audiodev", "none,id=snd", "-device", "AC97,audiodev=snd",
     "-monitor", "stdio", "-serial", "file:" + SERIAL,
     "-qmp", "unix:" + QMP_SOCK + ",server,nowait",
     "-display", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, cwd=ROOT)

def send(cmd, wait=0.4):
    qemu.stdin.write((cmd + "\n").encode()); qemu.stdin.flush(); time.sleep(wait)

def step(key):
    before = len(sel_lines())
    send("sendkey " + key, 0.2)
    dl = time.time() + 4
    while time.time() < dl:
        if len(sel_lines()) > before: return True
        time.sleep(0.15)
    return False

# ── QMP: true relative mouse injection (HMP mouse_move is absolute
# and degenerate with -display none — it never moves the guest cursor).
def qmp_connect(path, timeout=15):
    deadline = time.time() + timeout
    s = None
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            f = s.makefile("rw")
            json.loads(f.readline())          # greeting
            f.write('{"execute":"qmp_capabilities"}\n'); f.flush()
            json.loads(f.readline())          # capabilities ack
            return s, f
        except Exception:
            if s: s.close()
            time.sleep(0.5)
    raise RuntimeError("QMP connect failed")

try:
    qmp, qmpf = qmp_connect(QMP_SOCK)
except Exception as e:
    print("QMP:", e)
    qmp = None

def qmp_send(events):
    if not qmp: return
    qmpf.write(json.dumps({"execute": "input-send-event",
                           "arguments": {"events": events}}) + "\n")
    qmpf.flush()
    reply = json.loads(qmpf.readline())
    if "return" not in reply:
        print("QMP ERROR:", str(reply)[:200])

def mouse_rel(dx, dy):
    evs = []
    if dx: evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
    if dy: evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
    qmp_send(evs)

def mouse_btn(left, down):
    qmp_send([{"type": "btn",
               "data": {"button": "left" if left else "right",
                        "down": down}}])

def nav_row(target, label):
    cur = last_row()
    if cur is None: cur = 0
    guard = 0
    while cur != target and guard < 80:
        if not step("shift-b" if cur < target else "shift-a"):
            break
        nxt = last_row()
        if nxt is None or nxt == cur: break
        cur = nxt
        guard += 1
    print(label, "at row", cur, "(target", target, ")")
    return cur == target

results = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    time.sleep(1.5)
    wait_log("DESKTOP UP", 90)
    time.sleep(5)
    send("sendkey f2", 1.2)                         # open Files (home)
    time.sleep(0.6)
    for _ in range(5): send("sendkey shift-b", 0.25)  # -> Local Disk tile
    send("sendkey ret", 0.8)                        # open Local Disk
    time.sleep(1.5)
    results["nav_doom"] = nav_row(8, "nav")
    send("sendkey ret", 1.2)                        # launch #1
    results["pe_load_1"] = wait_count("PE: loading", 1, 60)
    results["doom_start_1"] = wait_log("[DOOM] doomgeneric_Create", 90)
    time.sleep(8)                                   # demo runs

    # ── Close via the [X] box ──────────────────────────────────────
    # Game window: 640×400 centered in 1920×1080 → x 640..1280,
    # y 340..740; title bar y 316..340; [X] box x 1258..1274,
    # y 320..336 → aim for center (1266, 328).
    #
    # Cursor starts centered (960, 540).  Two motion hazards make a
    # single big rel move land off-target: PS/2 splits >127 into many
    # packets (each scaled by the kernel's 0.6× fast-move curve), and
    # DOOM also rotates the view per X delta.  Fix: walk with ±1-unit
    # rel events (1:1 gain, 960×1080 ⇒ no rotation in menus... none
    # needed — the demo's menu is closed), then click.
    # NOTE: in game mode the kernel feeds RAW deltas to the game but
    # keeps accelerating the on-screen cursor the same way, so the
    # walk math is identical to the desktop.
    #
    #   dx needed: 1266-960 = +306 → 306 × (+1) events
    #   dy needed:  328-540 = -212 → 212 × (-1) events
    # CRITICAL: QEMU coalesces back-to-back rel events into PS/2
    # packets of up to ±127 units, and each big packet is scaled by
    # the kernel's 0.6× fast-move curve → massive undershoot.  Pace
    # the ±1 events at 25 ms (> PS/2 sample interval) so each becomes
    # its own packet and moves the cursor 1:1.  Even then coalescing
    # is probabilistic under KVM load, so after the first click we
    # VERIFY via the GAME CLOSED serial marker and retry with nudges.
    def walk(dx_total, dy_total, pace=0.025):
        sx = 1 if dx_total > 0 else -1
        sy = 1 if dy_total > 0 else -1
        for _ in range(abs(dx_total)):
            mouse_rel(sx, 0); time.sleep(pace)
        for _ in range(abs(dy_total)):
            mouse_rel(0, sy); time.sleep(pace)

    closed = False
    for attempt in range(4):
        if attempt == 0:
            walk(306, -212)              # 960,540 → (1266, 328)
        else:
            walk(14, -8)                 # nudge toward the box
        time.sleep(0.5)
        mouse_btn(True, True); time.sleep(0.15)
        mouse_btn(True, False); time.sleep(0.8)
        if wait_log("GAME CLOSED", 4):
            closed = True
            print("closed on attempt", attempt)
            break
    results["game_closed"] = closed
    time.sleep(2)
    send("screendump " + WORK + "/after_close.ppm", 0.8)

    # ── Relaunch: Enter again on the still-selected DOOM.EXE row ──
    send("sendkey ret", 1.5)
    results["pe_load_2"] = wait_count("PE: loading", 2, 60)
    results["doom_start_2"] = wait_count("[DOOM] doomgeneric_Create", 2, 90)
    time.sleep(6)
    send("screendump " + WORK + "/after_relaunch.ppm", 0.8)
    results["alive"] = qemu.poll() is None
    send("quit", 0.4)
    qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
finally:
    try: qemu.kill()
    except Exception: pass

log = readlog()
keep = ("PE:", "[DOOM]", "FATAL", "PANIC", "fault", "GAME")
for l in log.splitlines():
    if any(k in l for k in keep):
        print("|", l[:150])

print("\n=== RESULTS ===")
for k, v in results.items():
    print(f"{k:16} {'PASS' if v else 'FAIL'}")

relaunch_ok = (results.get("pe_load_2") and results.get("doom_start_2")
               and results.get("game_closed"))
nofault = "page fault" not in log.lower()
print("no page fault:", "PASS" if nofault else "FAIL")
print("\nRELAUNCH:", "PASS" if (relaunch_ok and nofault) else "FAIL")
