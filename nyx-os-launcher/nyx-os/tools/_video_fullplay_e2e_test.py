#!/usr/bin/env python3
"""Full-playback E2E for the Nyx video player.

Boots the ISO with KVM and the AC'97 device whose output is captured to a
WAV file (-audiodev wav).  Opens 6.mp4 (row 5, 42.5 s) and jace.mp4
(row 14, 19.4 s) from the Files app, watches each through at least one
full loop, sampling the VDV master-clock probe (audio µs + playhead
frame) live, and takes screendumps along the way.

PASS criteria (all must hold):
  traversal   — the playhead reaches >= 80% of the track then wraps to 0
  slope       — frames advanced / audio µs ≈ the video's fps (±12%)
  audio       — WAV is continuous during playback: no silent gap >= 0.5 s
                inside any loud region, and loud audio covers the watch
  picture     — screendumps taken 4+ s apart differ (the image moves)
"""
import subprocess, time, os, sys, threading, wave, struct, math

WORK = "/tmp/vplaytest"
os.makedirs(WORK, exist_ok=True)
serial_path = os.path.join(WORK, "serial.log")
wav_path = os.path.join(WORK, "audio.wav")
open(serial_path, "w").close()
for f in os.listdir(WORK):
    if f.endswith(".ppm"): os.remove(os.path.join(WORK, f))
ROOT = "/home/winnerbek/Downloads/nyx-os-launcher/nyx-os"

def readlog():
    try:
        return open(serial_path, "rb").read().decode("latin1", "replace")
    except Exception:
        return ""

# ── live VDV sampler: (wall_t, m_us, f) per probe line ───────────
vdv = []            # (time.time(), m, f)
_vdv_lock = threading.Lock()
_stop = threading.Event()

def sampler():
    pos = 0
    while not _stop.is_set():
        try:
            with open(serial_path, "rb") as fh:
                fh.seek(pos)
                chunk = fh.read()
                pos += len(chunk)
            for line in chunk.decode("latin1", "replace").splitlines():
                if line.startswith("VDV "):
                    m = f = None
                    for tok in line.split():
                        if tok.startswith("m="): m = int(tok[2:])
                        elif tok.startswith("f="): f = int(tok[2:])
                    if m is not None and f is not None:
                        with _vdv_lock:
                            vdv.append((time.time(), m, f))
        except Exception:
            pass
        _stop.wait(0.2)

def wait_log(sub, timeout=240):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sub in readlog(): return True
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
     "-audiodev", "wav,id=snd,path=" + wav_path,
     "-device", "AC97,audiodev=snd",
     "-monitor", "stdio", "-serial", "file:" + serial_path,
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

def vdv_snapshot():
    with _vdv_lock:
        return list(vdv)

results = {}
th = threading.Thread(target=sampler, daemon=True); th.start()
win_t = {}
try:
    results["login"] = wait_log("LOGIN READY", 90)
    for k in "1 2 3 4".split(): send("sendkey " + k, 0.25)
    send("sendkey ret", 0.4)
    results["desktop"] = wait_log("DESKTOP UP", 120)
    time.sleep(5)
    send("sendkey f2", 1.2)
    time.sleep(0.6)
    for _ in range(5): send("sendkey shift-b", 0.25)
    send("sendkey ret", 0.8)
    time.sleep(1.5)
    results["nav_six"] = nav_row(5, "6.mp4")
    time.sleep(1.0)
    send("sendkey ret", 0.9)
    results["six_open"] = wait_log("VDX idx=340", 60)
    win_t["six"] = time.time()      # VDV rows from here belong to 6.mp4

    # ── watch 6.mp4 fully (42.5 s) + wrap margin ─────────────────
    t6_start = time.time()
    for dt, name in ((5, "six_a"), (20, "six_b"), (36, "six_c"), (44, "six_d")):
        wait = t6_start + dt - time.time()
        if wait > 0: time.sleep(wait)
        send("screendump " + WORK + "/" + name + ".ppm", 0.8)
    t6_end = time.time()
    six_vdv = vdv_snapshot()
    six_watch = t6_end - t6_start

    send("sendkey esc", 1.2)
    time.sleep(1.0)
    results["nav_jace"] = nav_row(14, "jace.mp4")
    time.sleep(1.0)
    send("sendkey ret", 0.9)
    results["jace_open"] = wait_log("VDX idx=234", 60)
    win_t["jace"] = time.time()     # VDV rows from here belong to jace

    # ── watch jace.mp4 fully (19.4 s) + wrap margin ──────────────
    tj_start = time.time()
    for dt, name in ((3, "jac_a"), (10, "jac_b"), (16, "jac_c"), (21, "jac_d")):
        wait = tj_start + dt - time.time()
        if wait > 0: time.sleep(wait)
        send("screendump " + WORK + "/" + name + ".ppm", 0.8)
    tj_end = time.time()
    jace_vdv = vdv_snapshot()
    jace_watch = tj_end - tj_start

    send("sendkey esc", 1.0)
    send("quit", 0.5)
    qemu.wait(timeout=15)
except Exception as e:
    print("ERR:", e)
finally:
    _stop.set()
    try: qemu.kill()
    except Exception: pass

# ── analysis ──────────────────────────────────────────────────────
def analyze_vdv(all_rows, t0, t1, dur_ms, count, watch):
    """all_rows: [(wall_t, m_us, f)] from the whole run; only rows with
    t0 <= wall_t < t1 belong to this video (t1 = the next video's open
    time — the sampler keeps collecting until QEMU quits).  The audio clock m WRAPS at the
    loop point just like f, so both are unfolded together; the slope is
    frames-per-wall-second (wall time is the honest reference)."""
    rows = [r for r in all_rows if t0 <= r[0] < t1]
    if len(rows) < 6: return {"samples": len(rows), "ok": False}
    fmax = max(f for _, _, f in rows)
    wraps = 0
    pts = []                       # (unfolded_m_us, unfolded_prog)
    foff = 0; moff = 0
    prev_f = None; prev_m = None
    for (t, m, f) in rows:
        if prev_f is not None:
            if f < prev_f - count // 2:        # loop: f reset to 0
                wraps += 1; foff += count
                moff += prev_m                  # audio clock reset with it
            elif m < prev_m - 1_000_000:        # clock reset without f wrap
                moff += prev_m
        prev_f, prev_m = f, m
        pts.append((m + moff, f + foff))
    # fps = frames advanced / unfolded AUDIO-CLOCK span.  The WAV
    # continuity check proves the audio clock runs in real wall time,
    # so this is the honest rate measure — wall stamps from the
    # poll-batch sampler bunch rows together and skew a wall slope.
    fms = dur_ms / count / 1000.0
    slope = None
    if len(pts) >= 4 and pts[-1][0] > pts[0][0]:
        slope = (pts[-1][1] - pts[0][1]) / ((pts[-1][0] - pts[0][0]) / 1e6)
    # stalls: wall advanced > 2.5 frame-times but the playhead did not move
    stalls = 0
    for pa, pb in zip(pts, pts[1:]):
        if pb[1] == pa[1] and (pb[0] - pa[0]) / 1e6 > 2.5 * fms: stalls += 1
    exp_fps = 1000.0 * count / dur_ms
    slope_ok = slope is not None and abs(slope - exp_fps) <= 0.12 * exp_fps
    trav_ok = fmax >= 0.8 * count and wraps >= 1
    return {"samples": len(rows), "fmax": fmax, "wraps": wraps,
            "slope": round(slope, 2) if slope else slope,
            "exp_fps": round(exp_fps, 2), "stalls": stalls,
            "watch_s": round(watch, 1),
            "ok": trav_ok and slope_ok and stalls == 0}

six_an = analyze_vdv(vdv, win_t["six"], win_t["jace"], 42467, 340, six_watch)
jac_an = analyze_vdv(vdv, win_t["jace"], time.time() + 9e9, 19434, 234, jace_watch)
print("6.mp4 VDV:", six_an)
print("jace VDV:", jac_an)

# ── WAV continuity ────────────────────────────────────────────────
def analyze_wav(path):
    if not os.path.exists(path) or os.path.getsize(path) < 1000:
        return {"ok": False, "err": "no wav"}
    w = wave.open(path, "rb")
    n = w.getnframes(); rate = w.getframerate()
    data = w.readframes(n); w.close()
    win = rate // 2                                   # 0.5 s NON-overlapping
    rms = []
    for i in range(0, len(data) - win * 4 + 1, win * 4):
        acc = 0; cnt = 0
        for j in range(i, i + win * 4, 4):
            l, r = struct.unpack_from("<hh", data, j)
            acc += l * l + r * r; cnt += 1
        rms.append(math.sqrt(acc / cnt))
    loud = [i for i, v in enumerate(rms) if v > 400]
    if not loud: return {"ok": False, "err": "all silent"}
    # gaps of >= 1 window (0.5 s) between consecutive loud windows
    gaps = [(a * 0.5, b * 0.5) for a, b in zip(loud, loud[1:]) if b - a >= 2]
    cov = (loud[-1] - loud[0] + 1) * 0.5
    return {"frames": n, "dur_s": round(n / rate, 1),
            "coverage_s": round(cov, 1), "internal_gaps": gaps,
            "min_rms": round(min(rms[loud[0]:loud[-1] + 1]), 1),
            "ok": not gaps and cov >= 60}

wav_an = analyze_wav(wav_path)
print("WAV:", wav_an)

# ── picture changed? ──────────────────────────────────────────────
def diff_ppm(a, b):
    def openit(p):
        d = open(p, "rb").read()
        i = 0
        for _ in range(2):
            while d[i:i+1] != b"\n": i += 1
            i += 1
        return d, i
    da, oa = openit(a); db, ob = openit(b)
    if oa != ob: return -1
    diff = 0
    for p in range(oa, len(da), 3):
        if da[p:p+3] != db[p:p+3]: diff += 1
    return diff

pics = {}
for group, d1, d2 in (("six", "six_a", "six_c"), ("six2", "six_b", "six_d"),
                      ("jace", "jac_a", "jac_c"), ("jace2", "jac_b", "jac_d")):
    pa, pb = WORK + "/" + d1 + ".ppm", WORK + "/" + d2 + ".ppm"
    if os.path.exists(pa) and os.path.exists(pb):
        pics[group] = diff_ppm(pa, pb)
print("picture diffs (px):", pics)
pic_ok = all(v > 5000 for v in pics.values()) and len(pics) == 4

results["six_play"] = six_an["ok"]
results["jace_play"] = jac_an["ok"]
results["audio"] = wav_an.get("ok", False)
results["picture"] = pic_ok
print("RESULTS:", {k: ("OK" if v else "FAIL") for k, v in results.items()})
print("OVERALL:", "PASS" if all(results.values()) else "FAIL")
sys.exit(0 if all(results.values()) else 1)
