#!/usr/bin/env python3
"""E2E: CHESS chat sidebar — open chat with Q, type 'hint', verify the
PC replies; verify Enter (empty) leaves chat; then play 1. e4 and
confirm gameplay + the AI-move trace ring still work with chat present.

Pixel checks (canvas 1920×1080, sidebar x 8..468):
  - input box border turns green (CLR_OK) when chat has focus
  - typed text shows as white pixels inside the input box
  - your sent line appears as sky-blue (CLR_YOU) text in the history
  - the PC's hint appears as white text in the history region
  - after empty-Enter, focus drops (border back to frame color)

Mouse: same choreography as _chess_play_e2e_test.py (GAIN=3 unit events,
3 s settle — the game damps with a sum-preserving ÷3 accumulator).
"""
import subprocess, time, os, sys, socket, json

WORK = "/tmp/chesschat"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
open(serial_path, "w").close()
QMP_SOCK = os.path.join(WORK, "qmp.sock")
ROOT = "/home/winnerbek/Downloads/nyx-os-launcher/nyx-os"

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

def mouse_to(tx, ty, cur=[960, 540], settle=3.0):
    """Open-loop move (GAIN=3 unit events, 2 ms pacing).  Unreliable on
    its own — see move_abs() — kept for cheap parking moves."""
    GAIN, pace = 3, 0.002
    dx, dy = tx - cur[0], ty - cur[1]
    sx = 1 if dx > 0 else -1
    sy = 1 if dy > 0 else -1
    for _ in range(abs(dx) * GAIN):
        qmp_send([{"type": "rel", "data": {"axis": "x", "value": sx}}])
        time.sleep(pace)
    for _ in range(abs(dy) * GAIN):
        qmp_send([{"type": "rel", "data": {"axis": "y", "value": sy}}])
        time.sleep(pace)
    cur[0], cur[1] = tx, ty
    time.sleep(settle)

# ── closed-loop mouse core ────────────────────────────────────────
# QMP rel events drain with seconds-scale latency and the i8042 queue
# spills leftovers later — open-loop moves overshoot by 10s–100s of px
# after their settle, the assumed cursor position drifts, and clicks
# land on the WRONG square (the E2E "lost clicks" measured landing
# 141 px off target).  Cure: park in the clean panel region (no white
# pixels there except the game crosshair), measure the ACTUAL cursor
# position from a screendump, then send exactly the residual deltas.
PARK_X, PARK_Y = 1560, 200

# ── mouse helper: park-then-click with a margined residual move so
# board clicks never stack up and land on the wrong square (the DBG probe
# showed QMP deltas keep flowing for the full run — the late-failure risk
# is cumulative overshoot, not a dead guest).  We pull to the clean panel
# region first, measure the REAL crosshair, then drive exactly the residual
# to the target square.
PARK_X, PARK_Y = 1560, 200

def _panel_cx():
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
    """Drive the guest cursor to (tx,ty) from a known panel position."""
    for _ in range(3):
        mouse_to(PARK_X, PARK_Y, cur, settle=2.5)
        obs = _panel_cx()
        if obs is not None:
            cur[0], cur[1] = int(round(obs[0])), int(round(obs[1]))
            mouse_to(tx, ty, cur, settle=settle)
            return
        time.sleep(0.5)

def click(tx, ty, cur=[960, 540]):
    move_abs(tx, ty, cur)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.15)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.5)

def click_piece(f, r, cur):
    """Click a square and wait for the piece to select (CLR_SEL inside the
    source square).  QMP events occasionally go missing — retry once on a
    lost click instead of silently testing a phantom move."""
    x, y = sq_screen(f, r)
    rect = (BX + f*SQ, BY + r*SQ, BX + (f+1)*SQ, BY + (r+1)*SQ)
    for attempt in (1, 2):
        click(x, y, cur)
        time.sleep(1.0)
        if count_px(shot("chk.ppm"), rect, (79, 195, 247), 25) > 300:
            return True
        print("  click lost (sel miss), retrying:", f, r, "attempt", attempt)
    return False


def click(tx, ty, cur=[960, 540]):
    move_abs(tx, ty, cur)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.15)
    qmp_send([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.5)

# ── pixel helpers ─────────────────────────────────────────────────
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

def count_px(path, rect, want, tol):
    """Count pixels matching `want` (r,g,b) within tol inside rect."""
    w, h, rgb = load_ppm(path)
    x0, y0, x1, y1 = rect
    n = 0
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            o = (y*w + x)*3
            if (abs(rgb[o]-want[0]) <= tol and abs(rgb[o+1]-want[1]) <= tol
                    and abs(rgb[o+2]-want[2]) <= tol):
                n += 1
    return n

# Sidebar geometry (full-scale canvas coords)
BOX     = (16, 934, 452, 974)      # chat input box interior
HIST    = (12, 100, 464, 900)      # history area (below header)
CLR_OK     = (85, 204, 136)        # focused border
CLR_FRAME  = (68, 54, 114)         # unfocused border
CLR_TEXT   = (242, 236, 255)       # PC text
CLR_YOU    = (111, 200, 255)       # your text
CLR_TRACE  = (255, 184, 77)        # AI-move ring
CLR_DIM    = (156, 139, 200)       # mood face: calm head color
CLR_WARY   = (85, 204, 136)        # mood face: wary head color
FACE       = (330, 18, 432, 92)    # mood face box + word in the chat header

BX, BY, SQ = 480, 70, 120

def sq_screen(f, r):
    return (BX + f*SQ + SQ//2, BY + r*SQ + SQ//2)

def shot(name, wait=1.0):
    send("screendump " + WORK + "/" + name, wait)
    try:
        open(WORK + "/last_shot.ppm", "wb").write(open(WORK + "/" + name, "rb").read())
    except Exception: pass
    return WORK + "/" + name

def sq_rect(f, r):
    return (BX + f*SQ, BY + r*SQ, BX + (f+1)*SQ, BY + (r+1)*SQ)

def click_piece(f, r, cur):
    """Click a square and verify the piece got selected (CLR_SEL inside
    it).  QMP events occasionally go missing — retry once on a lost
    click instead of silently playing a phantom move."""
    x, y = sq_screen(f, r)
    for attempt in (1, 2):
        click(x, y, cur)
        time.sleep(1.0)
        if count_px(shot("chk.ppm"), sq_rect(f, r), (79, 195, 247), 25) > 300:
            return True
        print("click lost (sel miss), retrying:", f, r, "attempt", attempt)
    return False

results = {}
cur = [960, 540]
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    results["desktop"] = wait_log("DESKTOP UP", 120)
    time.sleep(5)
    send("sendkey f2", 1.5); time.sleep(1)
    for _ in range(5): step("shift-b")
    send("sendkey ret", 1.0); time.sleep(1.5)
    results["nav"] = nav_to_name("CHESS.EXE")
    send("sendkey ret", 1.5)
    results["pe_load"] = wait_log("PE: loading CHESS.EXE", 60)
    results["chess_up"] = wait_log("[CHESS] up", 60)
    time.sleep(3)

    # ── 1. baseline: chat unfocused ─────────────────────────────
    s0 = shot("s0.png")
    green0 = count_px(s0, BOX, CLR_OK, 14)
    results["box_unfocused"] = green0 < 30
    face0 = count_px(s0, FACE, CLR_DIM, 20)
    print("face calm px at boot:", face0)
    results["face_calm_boot"] = face0 > 200

    # ── mouse-heavy phases first: input is reliable early in a run;
    # QMP deltas keep flowing for the full run, but cumulative overshoot
    # can make later open-loop moves miss their target.  The DBG probe
    # (tools/_chess_dbg_probe.py) proved the guest keeps taking input.
    # After minutes of QMP queuing the keyboard-only chat phases come last.

    # ── M1. gameplay still works: 1. e4, AI replies with trace ──
    results["sel_e2"] = click_piece(4, 6, cur)
    click(*sq_screen(4, 4), cur)
    time.sleep(6)
    s8 = shot("s8.png")
    trace = count_px(s8, (BX, BY, BX + 8*SQ, BY + 8*SQ), CLR_TRACE, 14)
    print("trace px:", trace)
    results["ai_trace"] = trace >= 40

    # ── M2. mood face reacts to a real blunder ──────────────────
    # 2. Nf3 (mood still calm: evals near 0), then 3. Ne5?? — the
    # c6-knight captures, the PC's eval tanks and the face must flip
    # from calm (dim) to wary (green).
    ok1 = click_piece(6, 7, cur)             # g1
    click(*sq_screen(5, 5), cur)             # f3
    time.sleep(6)                            # AI reply (~calm)
    s9 = shot("s9.png")
    calm_mid = count_px(s9, FACE, CLR_DIM, 20)
    print("face calm px after Nf3:", calm_mid)
    results["face_calm_mid"] = ok1 and calm_mid > 200
    ok2 = click_piece(5, 5, cur)             # the f3 knight again
    click(*sq_screen(4, 3), cur)             # e5??
    time.sleep(6)                            # AI captures the knight
    s10 = shot("s10.png")
    calm_end = count_px(s10, FACE, CLR_DIM, 20)
    wary_end = count_px(s10, FACE, CLR_WARY, 20)
    print("face px after blunder: dim", calm_end, " wary", wary_end)
    results["face_flips"] = ok2 and calm_end < 80 and wary_end > 200

    # ── keyboard chat phases (no mouse needed) ──────────────────
    # ── 2. Q opens chat (green border) ──────────────────────────
    send("sendkey q", 0.5); time.sleep(0.8)
    s1 = shot("s1.png")
    green1 = count_px(s1, BOX, CLR_OK, 14)
    print("focus border px:", green0, "→", green1)
    results["q_opens"] = green1 > 150

    # ── 3. type 'hint' (white text in the box) ──────────────────
    text0 = count_px(s1, BOX, CLR_TEXT, 14)
    for k in "hint":
        send("sendkey " + k, 0.22)
    time.sleep(0.5)
    s2 = shot("s2.png")
    text1 = count_px(s2, BOX, CLR_TEXT, 14)
    print("typed text px:", text0, "→", text1)
    results["typed_shows"] = text1 > text0 + 60

    # ── 4. Enter sends → blue you-line + white reply appear ────
    blue0  = count_px(s2, HIST, CLR_YOU, 14)
    white0 = count_px(s2, HIST, CLR_TEXT, 14)
    send("sendkey ret", 0.5)
    time.sleep(4)                       # hint = depth-3 search
    s3 = shot("s3.png")
    blue1  = count_px(s3, HIST, CLR_YOU, 14)
    white1 = count_px(s3, HIST, CLR_TEXT, 14)
    print("hist blue px:", blue0, "→", blue1, " white:", white0, "→", white1)
    results["you_line"]  = blue1 > blue0 + 60
    results["pc_replies"] = white1 > white0 + 100

    # ── 5. ask for trivia — new white text must appear ─────────
    # (still focused: Enter sent 'hint' but only empty Enter leaves)
    for k in "trivia":
        send("sendkey " + k, 0.22)
    send("sendkey ret", 0.5)
    time.sleep(1.5)
    s4 = shot("s4.png")
    white_b = count_px(s4, HIST, CLR_TEXT, 14)
    print("white px after trivia:", white0, "→", white_b)
    results["trivia_replies"] = white_b > white1 + 100

    # ── 6. ask for mood — another new line ─────────────────────
    for k in "mood":
        send("sendkey " + k, 0.22)
    send("sendkey ret", 0.5)
    time.sleep(1.5)
    s5 = shot("s5.png")
    white_c = count_px(s5, HIST, CLR_TEXT, 14)
    print("white px after mood:", white_b, "→", white_c)
    results["mood_replies"] = white_c > white_b + 80

    # ── 7. taunt the PC — provoke reply ─────────────────────────
    for k in "you are slow":
        send("sendkey " + ("spc" if k == " " else k), 0.22)
    send("sendkey ret", 0.5)
    time.sleep(1.5)
    s6 = shot("s6.png")
    white_d = count_px(s6, HIST, CLR_TEXT, 14)
    print("white px after taunt:", white_c, "→", white_d)
    results["taunt_replies"] = white_d > white_c + 80

    # ── 8. empty Enter leaves chat (border back to frame) ──────
    send("sendkey ret", 0.5); time.sleep(0.8)
    s7 = shot("s7.png")
    green2 = count_px(s7, BOX, CLR_OK, 14)
    print("leave border px:", green1, "→", green2)
    results["enter_leaves"] = green2 < 30

    results["alive"] = qemu.poll() is None
    send("quit", 0.4)
    qemu.wait(timeout=10)
except Exception as e:
    print("ERR:", e)
    results["err"] = str(e)
finally:
    try: qemu.kill()
    except Exception: pass

log = readlog()
for l in log.splitlines():
    if any(k in l for k in ("PE:", "[CHESS]", "FATAL", "PANIC", "page fault")):
        print("|", l[:140])

print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
ok = all(v for k, v in results.items() if k != "err")
print("OVERALL:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
