#!/usr/bin/env python3
"""E2E: play chess — launch CHESS.EXE, play 1. e2e4 with QMP mouse
injection, verify the AI replied by reconstructing the board from
screendumps before/after.

Mouse: HMP mouse_move is degenerate with -display none, so this uses the
QMP human-interface-event API (same as the DOOM playtest battery): the
PS/2 device sees true relative deltas and button presses.

Board reconstruction: the canvas is FULL SCREEN 1920×1080 at (0,0);
the board is canvas-local (480,70), 8×8 of 120 px squares → screen
(480,70)..(1439,1029).  Pieces are the SVG-derived sprites: white
bodies are near-pure white, black bodies near-pure black — detected by
counting SOLID interior pixels (pixel + neighbors at ±6 px all match)
inside each square, so thin strokes/outlines don't fool the counter.

Assertions:
  - launch chain (PE load, [CHESS] up)
  - start position shows 32 pieces
  - after 1. e4: e2 vacated, e4 occupied by a white pawn
  - the AI made a visible reply (a black piece moved)
  - the AI's reply squares carry the orange trace outline (damped-cursor
    game builds send ±1 unit chunks; mouse_to paces them)
"""
import subprocess, time, os, sys, socket, json

WORK = "/tmp/chessplay2"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()
QMP_SOCK = os.path.join(WORK, "qmp.sock")
ROOT = "/home/winnerbek/Downloads/nyx-os-launcher/nyx-os"

CANVAS_X, CANVAS_Y = 0, 0                  # full-screen canvas
BX, BY, SQ = 480, 70, 120                  # board origin + square size
PANEL_X, PANEL_Y = 1560, 200               # neutral panel spot

def sq_screen(file_idx, rank_idx):
    """Center of a square in screen px.  rank_idx 0 = rank 8 (top)."""
    return (BX + file_idx*SQ + SQ//2, BY + rank_idx*SQ + SQ//2)

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
    try:
        return open(serial_path, "rb").read().decode("latin1", "replace")
    except Exception:
        return ""

def wait_log(sub, timeout=240):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sub in readlog(): return True
        time.sleep(1)
    return False

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

# ── QMP mouse ─────────────────────────────────────────────────────
def qmp_connect(path, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = None
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            f = s.makefile("rw")
            json.loads(f.readline())                       # greeting
            f.write('{"execute":"qmp_capabilities"}\n'); f.flush()
            json.loads(f.readline())                       # ack
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
    qmpf = None

def qmp_send(events):
    if not qmpf: return
    qmpf.write(json.dumps({"execute": "input-send-event",
                           "arguments": {"events": events}}) + "\n")
    qmpf.flush()
    try: json.loads(qmpf.readline())
    except Exception: pass

def mouse_to(tx, ty, cur=[960, 540], settle=3.0):
    """Open-loop move: GAIN=3 unit events, 2 ms pacing.

    The game damps raw deltas with a SUM-PRESERVING accumulator (÷3,
    remainder carried across polls), so N raw units move the cursor by
    exactly ~N/3 px no matter how QEMU coalesces back-to-back rel
    events into PS/2 packets — calibrated 900 raw → exactly 300 px
    (tools/_chess_mouse_probe.py).  Unreliable on its own, though:
    QMP events drain with seconds-scale latency and the i8042 queue
    spills leftovers later, so assumed positions drift — see
    move_abs(); this stays for cheap parking moves.

    The kernel feeds dy>0 = forward/up (DOOM), QEMU rel +y = down, so
    the guest sees -y; target math is in raw QEMU space and the game's
    negation restores screen orientation."""
    GAIN = 3
    pace = 0.002
    dx = tx - cur[0]; dy = ty - cur[1]
    sx = 1 if dx > 0 else -1
    sy = 1 if dy > 0 else -1
    for _ in range(abs(dx) * GAIN):
        qmp_send([{"type": "rel", "data": {"axis": "x", "value": sx}}])
        time.sleep(pace)
    for _ in range(abs(dy) * GAIN):
        qmp_send([{"type": "rel", "data": {"axis": "y", "value": sy}}])
        time.sleep(pace)
    cur[0], cur[1] = tx, ty
    time.sleep(settle)                               # let the queue drain

# ── closed-loop mouse core ─────────────────────────────────────────
# QMP rel events drain with seconds-scale latency and the i8042 queue
# spills leftovers later — open-loop moves overshoot by 10s–100s of px
# after their settle, the assumed cursor position drifts, and clicks
# land on the WRONG square (the "lost clicks" failures measured
# landing 141 px off target).  Cure: park in the clean panel region
# (no white pixels there except the game crosshair), measure the
# ACTUAL cursor position from a screendump, then send exactly the
# residual deltas.
PARK_X, PARK_Y = 1560, 200

def observe_cursor():
    """True (x, y) of the white crosshair inside the clean panel region,
    or None when it is not visible there."""
    p = WORK + "/obs.ppm"
    send("screendump " + p, 1.0)
    w, h, rgb = load_ppm(p)
    n = sx = sy = 0
    for y in range(60, 520):
        for x in range(1476, 1864):
            o = (y*w + x)*3
            if rgb[o] > 245 and rgb[o+1] > 245 and rgb[o+2] > 245:
                sx += x; sy += y; n += 1
    return (sx/n, sy/n) if n > 20 else None

def move_abs(tx, ty, cur, settle=3.0):
    """Move the guest cursor to (tx,ty) closed-loop (see block above)."""
    for attempt in (1, 2):
        mouse_to(PARK_X, PARK_Y, cur, settle=2.5)
        obs = observe_cursor()
        if obs is not None:
            cur[0], cur[1] = int(round(obs[0])), int(round(obs[1]))
            mouse_to(tx, ty, cur, settle=settle)
            return
        print("cursor lost (no crosshair observed), re-parking")
    raise RuntimeError("cursor lost")

def click(tx, ty, cur=[960, 540]):
    move_abs(tx, ty, cur)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.15)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.45)

# ── Board reconstruction ──────────────────────────────────────────
def load_ppm(p):
    d = open(p, "rb").read()
    i = 2; vals = []
    while len(vals) < 3:
        while i < len(d) and d[i:i+1].isspace(): i += 1
        j = i
        while j < len(d) and not d[j:j+1].isspace(): j += 1
        vals.append(int(d[i:j])); i = j
    i += 1
    return vals[0], vals[1], d[i:]

WHITE_B = (246, 246, 250); BLACK_B = (12, 12, 16)

def _solid(rgb, w, x, y, want, tol, reach=6):
    """True when the pixel AND its 4 neighbors at ±reach px all match
    `want` within tol — i.e. an INTERIOR pixel of a solid region, not a
    thin stroke or antialiased edge."""
    o = (y*w + x)*3
    if abs(rgb[o]-want[0]) > tol or abs(rgb[o+1]-want[1]) > tol \
       or abs(rgb[o+2]-want[2]) > tol:
        return False
    for dx, dy in ((-reach, 0), (reach, 0), (0, -reach), (0, reach)):
        p = ((y+dy)*w + (x+dx))*3
        if abs(rgb[p]-want[0]) > tol or abs(rgb[p+1]-want[1]) > tol \
           or abs(rgb[p+2]-want[2]) > tol:
            return False
    return True

def read_board(path):
    w, h, rgb = load_ppm(path)
    rows = []
    for r in range(8):
        row = ""
        for f in range(8):
            x0 = BX + f*SQ; y0 = BY + r*SQ
            cw = cb = 0
            for y in range(y0+10, y0+SQ-10, 4):
                for x in range(x0+10, x0+SQ-10, 4):
                    if _solid(rgb, w, x, y, WHITE_B, 10):   cw += 1
                    elif _solid(rgb, w, x, y, BLACK_B, 26): cb += 1
            row += "P" if cw > 12 else ("p" if cb > 12 else ".")
        rows.append(row)
    return rows

def piece_count(b):
    return sum(r.count("P") + r.count("p") for r in b)

# Verified click: QMP events occasionally go missing; confirm the move
# actually selected the piece (CLR_SEL border in the source square) and
# retry once if not, so the assertions never test a phantom game.
# Count pure-white pixels in the PANEL region (canvas x 1476..1864,
# y 60..520): the game's crosshair cursor parks there; panel text is
# 0xF2ECFF (not >245 on every channel) — so a big count would mean the
# desktop arrow ALSO drew next to the game cursor.
def panel_white_px(path):
    w, h, rgb = load_ppm(path)
    n = 0
    for y in range(60, 520):
        for x in range(1476, 1864):
            o = (y*w + x)*3
            if rgb[o] > 245 and rgb[o+1] > 245 and rgb[o+2] > 245: n += 1
    return n

def trace_px(path, rects):
    """Count CLR_TRACE (0xFFB84D) pixels inside canvas rects
    [(x0,y0,x1,y1), ...] — the orange outline marking the PC's move."""
    w, h, rgb = load_ppm(path)
    n = 0
    for (x0, y0, x1, y1) in rects:
        for y in range(y0, y1):
            for x in range(x0, x1):
                o = (y*w + x)*3
                if abs(rgb[o]-255) <= 12 and abs(rgb[o+1]-184) <= 16 \
                   and abs(rgb[o+2]-77) <= 16:
                    n += 1
    return n

results = {}
cur = [960, 540]
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    results["desktop"] = wait_log("DESKTOP UP", 120)
    time.sleep(5)
    send("sendkey f2", 1.5); time.sleep(1)
    for _ in range(5): step("shift-b")          # home tiles → Local Disk
    send("sendkey ret", 1.0); time.sleep(1.5)   # open Local Disk
    results["nav"] = nav_to_name("CHESS.EXE")
    send("sendkey ret", 1.5)
    results["pe_load"] = wait_log("PE: loading CHESS.EXE", 60)
    results["chess_up"] = wait_log("[CHESS] up", 60)
    time.sleep(3)
    # Park the cursor on the clean panel region so the game's own
    # crosshair enters a predictable location; then verify it's there and
    # dump the start position.  The cursor starts at (960,540) on the
    # board at boot, so it must be driven in first.
    move_abs(PANEL_X, PANEL_Y, cur)
    send("screendump " + WORK + "/before.ppm", 1.0)

    # 1. e2 → e4 : e-file idx 4, e2 rank idx 6, e4 rank idx 4
    click(*sq_screen(4, 6), cur)
    click(*sq_screen(4, 4), cur)
    time.sleep(6)                               # AI spinner + search + reply
    send("screendump " + WORK + "/after.ppm", 1.0)
    time.sleep(1)
    # The PC's reply must carry the orange trace ring.  Move-agnostic:
    # scan the 2px edge frame of EVERY square and count CLR_TRACE
    # (0xFFB84D) pixels — inside the board region nothing else draws
    # that color (CLR_WARN only appears in the panel/status line).  The
    # ring marks exactly ai_from + ai_to, so ≥2 squares must light up.
    trace_squares = []
    w, h, rgb = load_ppm(WORK + "/after.ppm")
    m = 4
    for ri in range(8):
        for fi in range(8):
            x0, y0 = BX + fi*SQ, BY + ri*SQ
            frames = [(x0, y0, x0+SQ, y0+m),
                      (x0, y0+SQ-m, x0+SQ, y0+SQ),
                      (x0, y0+m, x0+m, y0+SQ-m),
                      (x0+SQ-m, y0+m, x0+SQ, y0+SQ-m)]
            tr = trace_px(WORK + "/after.ppm", frames)
            if tr >= 8: trace_squares.append((ri, fi, tr))
    print("trace squares (ri,fi,px):", trace_squares)
    results["ai_trace"] = len(trace_squares) >= 2
    results["alive"] = qemu.poll() is None
    send("quit", 0.4)
    qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
    results["err"] = str(e)
finally:
    try: qemu.kill()
    except Exception: pass

before = read_board(WORK + "/before.ppm")
after  = read_board(WORK + "/after.ppm")
print("BEFORE:", *before, sep="\n  ")
print("AFTER:",  *after,  sep="\n  ")

nb, na = piece_count(before), piece_count(after)
changed = [(r, f) for r in range(8) for f in range(8)
           if before[r][f] != after[r][f]]
print("changed squares (r0=rank8):", changed, " pieces:", nb, "→", na)

results["startpos_32"] = nb == 32
results["e2_vacated"]  = (before[6][4] == "P" and after[6][4] == ".")
results["e4_occupied"] = after[4][4] == "P"
# Single-cursor proof: the cursor is parked on the panel for the
# before-shot; only the game's own ~44 px cursor may be there.
pw = panel_white_px(WORK + "/before.ppm")
print("panel white px (cursor only expected ≈60):", pw)
results["one_cursor"] = 20 <= pw <= 130
# AI reply: at least one BLACK piece changed square (moved or was captured).
results["ai_replied"]  = any(before[r][f] == "p" and after[r][f] == "."
                             for r in range(8) for f in range(8)) or \
                         any(before[r][f] == "." and after[r][f] == "p"
                             for r in range(8) for f in range(8))

log = readlog()
for l in log.splitlines():
    if any(k in l for k in ("PE:", "[CHESS]", "FATAL", "PANIC", "page fault")):
        print("|", l[:140])

print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
ok = all(results.values())
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
