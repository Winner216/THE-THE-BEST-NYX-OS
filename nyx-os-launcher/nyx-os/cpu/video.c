// cpu/video.c — VESA video player for Nyx OS
//
// Plays MP4 container files whose video track is MJPEG — the only
// codec the kernel can decode.  Parses the container metadata (brand,
// resolution, duration, codec), builds a frame index by scanning the
// mdat payload for JPEG streams, and plays them back at the frame
// rate derived from the container's mdhd timescale.  Non-MJPEG files
// (H.264 etc.) show their metadata with a "codec not supported"
// message instead of crashing.
//
// Transport: Space = play/pause, Left/Right = seek, ESC = exit
// (handled by desktop.c via video_return_mode, same as imgview).
// Audio: m = mute toggle, -/+ = volume (glides via audio.c's smoothed
// gain), n = mono/stereo downmix toggle.  The status line shows the
// current VOL level or MUTE, plus MONO when downmixed.
//
// DECODE-AHEAD: the player is double-buffered.  After presenting a
// frame, the main loop (video_pump_ahead, called from
// desktop_video_pump) decodes the NEXT frame into the back buffer
// during the current frame's window, so the present at the frame's
// PTS is a pure blit (index flip, no decode).  The present path also
// gates on the master clock (vd_master_us vs vd_frame_pts_us).

#include <video.h>
#include <vesa.h>
#include <desktop.h>
#include <mouse.h>
#include <jpegdec.h>
#include <audio.h>
#include <timer.h>
#include <io.h>
#include <stdint.h>
#include <stddef.h>

// Serial trace helper (COM1): VDX = open/parse, VDV = playback
// master-clock probe, MP3A = the async MP3 worker's own lines.
static void vd_trace(const char* s) {
    for (; *s; s++) {
        int spins = 0;
        while ((inb(0x3F8 + 5) & 0x20) == 0 && spins++ < 200000) { }
        outb(0x3F8, (uint8_t)*s);
    }
}
static void vd_trace_num(uint32_t v) {
    // BUGFIX: emit digits as raw bytes.  The old code handed
    // vd_trace(&tmp[--i]) a NON-NUL-terminated buffer, so it walked
    // past the digits into stack garbage and the VDX traces came out
    // corrupted ("idx=3<garbage>43<garbage>...").
    char tmp[12]; int i = 0;
    if (v == 0) { vd_trace("0"); return; }
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) {
        int sp = 0;
        while ((inb(0x3F8 + 5) & 0x20) == 0 && sp++ < 200000) { }
        outb(0x3F8, (uint8_t)tmp[--i]);
    }
}

// ─── Player state ────────────────────────────────────────────────
static int           vd_active = 0;
static const uint8_t* vd_data  = NULL;   // pointer into filesystem content
static int           vd_len   = 0;
static char          vd_title[64] = {0}; // file name shown in title bar
static char          vd_brand[8]  = {0}; // ftyp major brand (isom/mp42/...)
static char          vd_codec[8]  = {0}; // video sample entry format
static int           vd_width  = 0;      // coded width (tkhd, 16.16)
static int           vd_height = 0;      // coded height
static int           vd_dur_ms = 0;      // duration from mdhd
static int           vd_mjpeg  = 0;      // 1 = playable MJPEG, 0 = other
static int           vd_meta_frames = 0; // sample count from stsz (display)

// MJPEG frame index — vd_frame_off[] are ABSOLUTE offsets into the
// file (mdat payload base added at scan time) so decoding is a plain
// vd_data + off with no base bookkeeping.
#define VD_MAX_FRAMES 2048
static int           vd_frame_count = 0;
static int           vd_frame_off[VD_MAX_FRAMES];
static int           vd_frame_len[VD_MAX_FRAMES];

// Decode scratch — frames are capped at 1280x720 RGBA
#define VD_MAX_W 1280
#define VD_MAX_H 720
// 64 B-aligned: RGBA rows are all 64 B-multiples (width×4), so an
// aligned base keeps every decode row on a cache-line boundary.
//
// DOUBLE BUFFERING: vd_pixbuf[0] and [1] are the two frame buffers;
// vd_front_idx selects which one is on screen.  The front descriptors
// (vd_decoded_frame / vd_dec_w/h / vd_decode_ok) describe
// vd_pixbuf[vd_front_idx].  The decode-ahead path fills the OTHER
// buffer with the next frame during the current frame's window, and
// the present "promotes" it by flipping the index — zero data
// movement, so presenting at PTS is a pure blit.
static uint8_t       vd_pixbuf[2][VD_MAX_W * VD_MAX_H * 4] __attribute__((aligned(64)));
static int           vd_front_idx = 0;             // buffer on screen (0/1)
static int           vd_dec_w = 0, vd_dec_h = 0;   // current decoded dims
static int           vd_decoded_frame = -1;        // last frame decoded into buf
static int           vd_decode_ok = 0;             // 1 = last decode succeeded
static int           vd_blitted_frame = -1;        // frame whose pixels are on screen
// Back-buffer descriptors — the frame decoded AHEAD of the playhead.
static int           vd_back_frame = -2;           // frame in the other buffer
static int           vd_back_ok = 0;               // 1 = back holds a valid decode
static int           vd_back_w = 0, vd_back_h = 0; // dims of the back decode
static int           vd_last_vx = 0, vd_last_vy = 0,
                     vd_last_vw = 0, vd_last_vh = 0; // last video-area geometry

// Playback state
static int           vd_playing = 1;
static int           vd_frame   = 0;               // current frame index
static int           vd_fps     = 12;              // fallback, refined from mdhd
static int           vd_play_ms = 0;               // accumulator inside frame

// ─── Master clock + PTS ─────────────────────────────────────────
// ONE timeline drives the playhead (see vd_master_us):
//   • AUDIO clock (µs PTS of the decoded blocks) is master whenever
//     sound is playing — the sample counter advances in real time
//     no matter how long the main loop spends decoding.
//   • TSC wall clock (cpu/timer.c, µs) is master when there is no
//     audio (or it finished) — the video clock free-runs smoothly
//     instead of in 20 ms PIT steps.
// vd_wall_base_us anchors the TSC clock at playback (re)start, so
// the free-run position is timer_us() - vd_wall_base_us.
static uint64_t vd_wall_base_us = 0;

// PTS of frame `f` in µs: uniform frame rate (MJPEG frames are all
// the same duration), scaled by the container's duration.  This is
// the timestamp ATTACHED to every video frame; the audio-master sync
// below compares it against the audio blocks' PTS on the same µs
// timeline.
static inline uint64_t vd_frame_pts_us(int f) {
    if (vd_frame_count < 2 || vd_dur_ms <= 0) return 0;
    return (uint64_t)f * (uint64_t)vd_dur_ms * 1000 / vd_frame_count;
}

// The master timeline right now, in µs since the video started
// (loops re-anchor).  Audio is master while it plays; otherwise the
// TSC wall clock since vd_wall_base_us.
static inline uint64_t vd_master_us(void) {
    if (audio_has_audio() && audio_is_playing())
        return (uint64_t)audio_elapsed_us();
    return timer_us() - vd_wall_base_us;
}

// ─── Audio-mastered A/V sync ────────────────────────────────────
// While a PCM sidecar is playing, the audio clock (speaker IRQ,
// real time) is the master timeline: video_tick() advances the
// playhead only when the sound crosses into the next frame's time.
// When a slow decode lets the audio run AHEAD, the frames in between
// are dropped (MJPEG frames are ALL non-reference — each JPEG is
// decoded independently, no motion compensation, no forward
// references — so skipping any of them is always safe); when the
// playhead instead runs AHEAD of the sound (backward-seek
// truncation, a clock hiccup, an external jump), the picture briefly
// HOLDS — repeats the last frame — and only re-anchors if the lead
// grows past the per-video hold window.
//
// Both sync windows are defined in WALL-CLOCK MILLISECONDS and
// converted to frames for each video's own frame interval (see
// video_tick() and vd_count_dropped()), so 8 fps and 24 fps content
// get proportionally similar tolerances:
//   VD_AHEAD_HOLD_MS — playhead lead (picture ahead of sound) that
//                      holding absorbs before the playhead re-anchors
//                      to the audio position.  250 ms ≈ 2 frames at
//                      8 fps, ≈ 6 frames at 24 fps.
//   VD_DROP_DEAD_MS  — minimum wall-clock span of a frame skip
//                      (sound ahead of the picture) before it is
//                      tallied in vd_dropped.  125 ms ≈ 1 frame at
//                      8 fps, ≈ 3 frames at 24 fps.
#define VD_AHEAD_HOLD_MS 250
#define VD_DROP_DEAD_MS  125
static int           vd_dropped = 0;               // frames dropped by audio-sync

// Audio sidecar (8-bit unsigned PCM, see tools/transcode-videos.py)
static const uint8_t* vd_audio      = NULL;
static int            vd_audio_len  = 0;
static int            vd_audio_rate = 0;

// ─── In-container MP3 audio ──────────────────────────────────────
// tools/transcode-videos.py muxes the video's audio track into the
// MP4 as an MP3 elementary stream.  The MP3 samples are INTERLEAVED
// with the MJPEG frames in mdat (qtmux chunks), so they are assembled
// into a contiguous elementary stream at open time by walking the
// audio track's sample tables (stco/stsc/stsz).  That stream is then
// decoded in the background by cpu/mp3async.c and played through the
// AC'97 path — no more 6 kHz robot-voice sidecar.
#define VD_MP3_MAX 1500000     // 1.5 MiB elementary-stream scratch
static uint8_t  vd_mp3_buf[VD_MP3_MAX];
static int      vd_mp3_len  = 0;   // assembled stream size (0 = none)
// Scratch for the audio track's sample tables (per-sample sizes can
// reach thousands of entries; cap the tables like the frame index).
#define VD_MP3_MAX_CHUNKS 4096
#define VD_MP3_MAX_SAMPLES 8192
static int vd_mp3_chunk_off[VD_MP3_MAX_CHUNKS];
static int vd_mp3_sc_first[VD_MP3_MAX_CHUNKS];
static int vd_mp3_sc_per[VD_MP3_MAX_CHUNKS];
static int vd_mp3_samp_size[VD_MP3_MAX_SAMPLES];

// ─── Adaptive decode (ECO ladder) ────────────────────────────────
// If a full-res frame decodes slower than the playback frame
// interval, the player steps DOWN one rung at a time to keep
// playback fluid:
//   level 0: full          704×704   (jpeg_decode)
//   level 1: vertical-half 704×352   (jpeg_decode_eco_v — drop odd
//            MCU rows only, keeping ALL horizontal detail)
//   level 2: both-axes     352×352   (jpeg_decode_eco — last resort)
//
// Measured with tools/bench/ (the kernel's own decoder booted under
// QEMU TCG, timed with the same PIT math used here):
//                   full 704²   eco_v 704×352   eco2 352×352
//   QEMU TCG        ~168 ms      ~97 ms         ~44 ms
//   native host     ~22 ms       ~11 ms         ~5 ms
// At 8 fps the frame interval is 125 ms, so under TCG full-res
// cannot stay fluid and the ladder settles at vertical-half
// (97 ms < 125 ms keeps up, with full horizontal detail).  A fast
// host (KVM/native, ~22 ms) stays at full-res with huge headroom.
//
// Thresholds (dead band = budget/2 .. budget, i.e. 62..125 ms at 8 fps):
//   descend when dec_ms >= budget                 (125 ms @8 fps)
//     Benchmark data (tools/bench, kernel's own decoder under QEMU TCG)
//     showed full-res oscillating 138..169 ms — the old budget+1/4
//     threshold (156 ms) sat INSIDE that noise band, so the 4-consecutive
//     rule could take seconds to trip.  Fast hosts decode full-res at
//     ~22 ms, a 5× spike below budget, so the +25% margin bought nothing
//     there.  Plain budget is the correct boundary: any host over the
//     interval steps down within 4 frames, and fast hosts never do.
//   ascend  when dec_ms <= budget / 2           (≈62  ms @8 fps)
//     Full-res costs ≈2× a vertical-half frame (half the pixels), so
//     only step up when the CURRENT rung decodes within half the
//     interval — that guarantees the full rung would fit.  This is
//     what stops TCG oscillation: vertical-half at 72..92 ms is inside
//     the dead band (never steps up to 160 ms full, never steps down
//     to 50 ms both-axes), while a fast host at 22 ms is well under
//     62 ms and keeps full-res.
// 4 consecutive slow decodes step down one level; 8 consecutive
// comfortably-fast decodes step back up.
static int  vd_eco       = 0;   // 0 = full, 1 = vertical-half, 2 = both
static int  vd_eco_slow  = 0;   // consecutive slow-decode counter
static int  vd_eco_fast  = 0;   // consecutive fast-decode counter

// Global PIT tick counter (defined in cpu/isr.c) — declared here so
// (volatile: written by the ISR; matches include/isr.h)
// the sub-tick timing helper below can read it; the player also
// re-externs it near video_tick() for the once-per-tick guard.
extern volatile uint32_t tick;

// ─── MP4 box parsing helpers ────────────────────────────────────
static uint32_t vd_rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static int vd_fcc(const uint8_t* p, const char* t) {
    return p[0]==t[0] && p[1]==t[1] && p[2]==t[2] && p[3]==t[3];
}

// Find a top-level box of the given type in [d, d+len).
// Returns non-NULL on success; *po = payload offset RELATIVE TO d,
// *ps = payload size.  (The returned pointer is only a truth value —
// callers must address payloads as `d + *po`, never as the return.)
static const uint8_t* vd_find_box(const uint8_t* d, int len,
                                  const char* type, int* po, int* ps) {
    int off = 0;
    while (off + 8 <= len) {
        uint32_t sz = vd_rd32(d + off);
        if (sz == 1) break;                       // 64-bit size: unsupported
        if (sz < 8) break;
        if (sz == 0) sz = (uint32_t)(len - off);  // extends to end of file
        if (off + (int)sz > len) break;
        if (vd_fcc(d + off + 4, type)) {
            if (po) *po = off + 8;
            if (ps) *ps = (int)sz - 8;
            return d + off;
        }
        off += (int)sz;
    }
    return NULL;
}

// Deep-search for a box by recursing into container boxes up to `depth`
// levels.  Same contract as vd_find_box: the *po/*ps outputs are
// RELATIVE TO d (offsets accumulate through the recursion), so callers
// address payloads as `d + *po`.  The return value is a truth flag.
static const uint8_t* vd_find_deep(const uint8_t* d, int len,
                                   const char* type, int depth,
                                   int* po, int* ps) {
    int off = 0;
    while (off + 8 <= len) {
        uint32_t sz = vd_rd32(d + off);
        if (sz == 1 || sz < 8) break;
        if (sz == 0) sz = (uint32_t)(len - off);
        if (off + (int)sz > len) break;
        if (vd_fcc(d + off + 4, type)) {
            if (po) *po = off + 8;
            if (ps) *ps = (int)sz - 8;
            return d + off;
        }
        if (depth > 0) {
            int qo = 0, qs = 0;
            if (vd_find_deep(d + off + 8, (int)sz - 8, type, depth - 1,
                             &qo, &qs)) {
                if (po) *po = off + 8 + qo;
                if (ps) *ps = qs;
                return d + off;   // truth value only
            }
        }
        off += (int)sz;
    }
    return NULL;
}

// Parse tkhd for coded width/height (16.16 fixed point at payload end).
// Ignores non-positive dims (audio tracks report 0x0).
static void vd_parse_tkhd(const uint8_t* p, int len) {
    if (len < 84) return;
    int ver = p[0];
    // tkhd v0:  width at +76, height at +80 ; v1: +88/+92
    int woff = (ver == 1) ? 88 : 76;
    if (woff + 8 > len) return;
    int w = (int)(vd_rd32(p + woff) >> 16);
    int h = (int)(vd_rd32(p + woff + 4) >> 16);
    if (w > 0 && h > 0) { vd_width = w; vd_height = h; }
}

// Parse mdhd for timescale/duration → duration in ms, and a frame-rate
// hint from common video timescales.
static void vd_parse_mdhd(const uint8_t* p, int len) {
    if (len < 20) return;
    int ver = p[0];
    int ts_off = (ver == 1) ? 20 : 12;
    int du_off = ts_off + 4;
    if (du_off + 4 > len) return;
    uint32_t ts  = vd_rd32(p + ts_off);
    uint32_t dur = vd_rd32(p + du_off);
    if (ts > 0 && dur > 0) vd_dur_ms = (int)((uint64_t)dur * 1000 / ts);
    if (vd_fps == 12) {
        if (ts == 90000) vd_fps = 24;
        else if (ts == 30000) vd_fps = 30;
    }
}

// Parse stsd to grab the first sample-entry format (the track codec)
// into out4[0..3] (NUL-terminated).  stsd: version/flags(4) +
// entry_count(4) + entries; first entry: size(4) + format(4)...
static void vd_parse_stsd(const uint8_t* p, int len, char* out4) {
    out4[0] = 0;
    if (len < 16) return;
    int ent_off = 12;  // 8 header + 4 count + 4 entry size
    out4[0] = p[ent_off+0]; out4[1] = p[ent_off+1];
    out4[2] = p[ent_off+2]; out4[3] = p[ent_off+3];
    out4[4] = 0;
}

// ─── MP3 audio-track extraction ──────────────────────────────────
// Walk the moov payload for an AUDIO trak whose stsd fourcc is
// ".mp3" and assemble its samples (mdat chunks, described by the
// audio track's stco/stsc/stsz) into vd_mp3_buf.  `d` = moov payload.
// The chunk offsets in stco are ABSOLUTE file offsets, so the mdat
// base is NOT added — the assembled copies are plain file reads.
static void vd_extract_mp3(const uint8_t* d, int len) {
    vd_mp3_len = 0;
    int off = 0;
    while (off + 8 <= len) {
        uint32_t sz = vd_rd32(d + off);
        if (sz == 1 || sz < 8) break;
        if (sz == 0) sz = (uint32_t)(len - off);
        if (off + (int)sz > len) break;
        if (vd_fcc(d + off + 4, "trak")) {
            char tmp[8] = {0};
            int p = 0, ps = 0;
            if (vd_find_deep(d + off, (int)sz, "stsd", 4, &p, &ps))
                vd_parse_stsd(d + off + p, ps, tmp);
            if (tmp[0] && vd_fcc((const uint8_t*)tmp, ".mp3")) {
                // stco: version/flags(4) + entry_count(4) + offsets
                // stsc: version/flags(4) + entry_count(4) + entries of
                //       (first_chunk, samples_per_chunk, sdi)
                // stsz: version/flags(4) + sample_size(4) + count(4) +
                //       then per-sample sizes if sample_size == 0
                int co = 0, cs = 0, so = 0, ss = 0, zo = 0, zs = 0;
                if (!vd_find_deep(d + off, (int)sz, "stco", 4, &co, &cs) ||
                    !vd_find_deep(d + off, (int)sz, "stsc", 4, &so, &ss) ||
                    !vd_find_deep(d + off, (int)sz, "stsz", 4, &zo, &zs))
                    break;
                const uint8_t* stco = d + off + co;
                const uint8_t* stsc = d + off + so;
                const uint8_t* stsz = d + off + zo;
                int nchunks = (int)vd_rd32(stco + 4);
                int nsc     = (int)vd_rd32(stsc + 4);
                int fixed   = (int)vd_rd32(stsz + 4);
                int nsamp   = (int)vd_rd32(stsz + 8);
                if (nchunks <= 0 || nsc <= 0 || nsamp <= 0 ||
                    nchunks > VD_MP3_MAX_CHUNKS || nsc > VD_MP3_MAX_CHUNKS ||
                    nsamp > VD_MP3_MAX_SAMPLES ||
                    cs < 8 + 4 * nchunks || ss < 8 + 12 * nsc ||
                    (fixed == 0 && zs < 12 + 4 * nsamp))
                    break;
                for (int i = 0; i < nchunks; i++)
                    vd_mp3_chunk_off[i] = (int)vd_rd32(stco + 8 + 4 * i);
                for (int i = 0; i < nsc; i++) {
                    vd_mp3_sc_first[i] = (int)vd_rd32(stsc + 8 + 12 * i);
                    vd_mp3_sc_per[i]   = (int)vd_rd32(stsc + 12 + 12 * i);
                }
                for (int i = 0; i < nsamp; i++)
                    vd_mp3_samp_size[i] = fixed ? fixed : (int)vd_rd32(stsz + 12 + 4 * i);

                // Walk chunks; each chunk holds sc_per samples (from
                // the stsc entry whose first_chunk <= chunk).
                int si = 0;   // sample index
                int ci = 0;   // stsc entry index
                for (int ch = 0; ch < nchunks && si < nsamp; ch++) {
                    while (ci + 1 < nsc && vd_mp3_sc_first[ci + 1] <= ch + 1) ci++;
                    int per = vd_mp3_sc_per[ci];
                    int foff = vd_mp3_chunk_off[ch];
                    for (int k = 0; k < per && si < nsamp; k++) {
                        int ssz = vd_mp3_samp_size[si];
                        if (vd_mp3_len + ssz > VD_MP3_MAX) { si = nsamp; break; }
                        if (foff + ssz > vd_len) { si = nsamp; break; }
                        for (int b = 0; b < ssz; b++)
                            vd_mp3_buf[vd_mp3_len + b] = vd_data[foff + b];
                        vd_mp3_len += ssz;
                        foff += ssz;
                        si++;
                    }
                }
                break;   // audio track assembled
            }
        }
        off += (int)sz;
    }
    vd_trace("VDX mp3len="); vd_trace_num((uint32_t)vd_mp3_len); vd_trace("\n");
}

// Is a fourcc a video codec (as opposed to audio like mp4a/soun)?
static int vd_is_video_codec(const char* c) {
    return vd_fcc((const uint8_t*)c, "jpeg") ||
           vd_fcc((const uint8_t*)c, "mjpa") ||
           vd_fcc((const uint8_t*)c, "mjpg") ||
           vd_fcc((const uint8_t*)c, "avc1") ||
           vd_fcc((const uint8_t*)c, "h264") ||
           vd_fcc((const uint8_t*)c, "mp4v") ||
           vd_fcc((const uint8_t*)c, "hev1") ||
           vd_fcc((const uint8_t*)c, "hvc1");
}

// Index MJPEG frames from the VIDEO track's sample tables (stco /
// stsc / stsz) instead of scanning mdat for JPEG SOI/EOI byte pairs.
// Since the transcoder muxes the MP3 soundtrack INTO mdat (chunks
// interleaved between the MJPEG frames), the old byte scan no longer
// works: Huffman-coded audio data can contain stray FF D8 / FF D9
// pairs, which registered as bogus "frames" — and real frames the
// scan then walked past were swallowed.  That inflated the frame
// count (PTS math is frame time = duration / count, so every bogus
// frame shifted the whole A/V timeline), made playback try to decode
// garbage ("Frame decode failed" flicker), and silently skipped real
// frames.  Sample tables give exact sample boundaries — one MJPEG
// sample is exactly one JPEG — so nothing outside the video track
// can leak into the index.
//
// `trak` points at the adopted video trak BOX (header included), `len`
// is its size.  stco chunk offsets are ABSOLUTE file offsets, so the
// index holds vd_data-addressable positions just like the old scan.
// Returns the number of frames indexed (0 = tables unusable/missing;
// the caller falls back to the mdat byte scan).
static int vd_index_video_tables(const uint8_t* trak, int len) {
    int co = 0, cs = 0, so = 0, ss = 0, zo = 0, zs = 0;
    if (!vd_find_deep(trak, len, "stco", 4, &co, &cs) ||
        !vd_find_deep(trak, len, "stsc", 4, &so, &ss) ||
        !vd_find_deep(trak, len, "stsz", 4, &zo, &zs))
        return 0;
    const uint8_t* stco = trak + co;
    const uint8_t* stsc = trak + so;
    const uint8_t* stsz = trak + zo;
    int nchunks = (int)vd_rd32(stco + 4);   // ver/flags(4) + count(4)
    int nsc     = (int)vd_rd32(stsc + 4);
    int fixed   = (int)vd_rd32(stsz + 4);   // 0 = per-sample sizes follow
    int nsamp   = (int)vd_rd32(stsz + 8);
    if (nchunks <= 0 || nsc <= 0 || nsamp <= 0) return 0;
    // A chunk holds >= 1 sample, so chunks never exceed samples; cap
    // both at the frame-index size (clamped BEFORE the table-size
    // bounds checks so only the entries we actually read are checked).
    if (nchunks > VD_MAX_FRAMES) nchunks = VD_MAX_FRAMES;
    if (nsamp   > VD_MAX_FRAMES) nsamp   = VD_MAX_FRAMES;
    if (cs < 8 + 4 * nchunks || ss < 8 + 12 * nsc || zs < 12 ||
        (fixed == 0 && zs < 12 + 4 * nsamp))
        return 0;
    int count = 0;
    int si = 0, ci = 0;
    for (int ch = 0; ch < nchunks && si < nsamp && count < VD_MAX_FRAMES; ch++) {
        while (ci + 1 < nsc &&
               vd_rd32(stsc + 8 + 12 * (ci + 1)) <= (uint32_t)(ch + 1))
            ci++;   // active stsc entry for this chunk
        int per  = (int)vd_rd32(stsc + 12 + 12 * ci);
        int foff = (int)vd_rd32(stco + 8 + 4 * ch);
        for (int k = 0; k < per && si < nsamp && count < VD_MAX_FRAMES; k++) {
            int ssz = fixed ? fixed : (int)vd_rd32(stsz + 12 + 4 * si);
            si++;
            if (ssz < 4 || foff < 0 || foff + ssz > vd_len) { foff += ssz; continue; }
            // Belt-and-braces: the sample should BE one JPEG (SOI ..
            // EOI).  If a muxer ever pads a sample, resync to the
            // first complete JPEG inside it so padding can never
            // poison a decode; samples with no JPEG are dropped.
            int b = foff, e = foff + ssz;
            while (b + 1 < e && !(vd_data[b] == 0xFF && vd_data[b + 1] == 0xD8)) b++;
            if (b + 1 >= e) { foff += ssz; continue; }        // no SOI
            int j = b + 2;
            while (j + 1 < e && !(vd_data[j] == 0xFF && vd_data[j + 1] == 0xD9)) j++;
            if (j + 1 >= e) { foff += ssz; continue; }        // no EOI
            vd_frame_off[count] = b;
            vd_frame_len[count] = j + 2 - b;
            count++;
            foff += ssz;
        }
    }
    return count;
}

// Walk the trak boxes inside a moov payload; adopt the FIRST video
// track's tkhd/mdhd/stsd (skipping audio-only tracks).  `d` = moov
// payload start, len = payload size.
static void vd_parse_traks(const uint8_t* d, int len) {
    int off = 0;
    while (off + 8 <= len) {
        uint32_t sz = vd_rd32(d + off);
        if (sz == 1 || sz < 8) break;
        if (sz == 0) sz = (uint32_t)(len - off);
        if (off + (int)sz > len) break;
        if (vd_fcc(d + off + 4, "trak")) {
            char tmp[8] = {0};
            int p = 0, ps = 0;
            if (vd_find_deep(d + off, (int)sz, "stsd", 4, &p, &ps))
                vd_parse_stsd(d + off + p, ps, tmp);
            if (tmp[0] && vd_is_video_codec(tmp)) {
                for (int i = 0; i < 5; i++) vd_codec[i] = tmp[i];
                vd_mjpeg = vd_fcc((const uint8_t*)tmp, "jpeg") ||
                           vd_fcc((const uint8_t*)tmp, "mjpa") ||
                           vd_fcc((const uint8_t*)tmp, "mjpg");
                if (vd_find_deep(d + off, (int)sz, "tkhd", 3, &p, &ps))
                    vd_parse_tkhd(d + off + p, ps);
                if (vd_find_deep(d + off, (int)sz, "mdhd", 3, &p, &ps))
                    vd_parse_mdhd(d + off + p, ps);
                if (vd_find_deep(d + off, (int)sz, "stsz", 3, &p, &ps)) {
                    // stsz: version/flags(4) sample_size(4) sample_count(4)
                    if (ps >= 12) vd_meta_frames = (int)vd_rd32(d + off + p + 8);
                }
                // Frame index from the video track's sample tables
                // (see vd_index_video_tables — the mdat SOI/EOI scan
                // can't be used once audio chunks share mdat).
                if (vd_mjpeg)
                    vd_frame_count = vd_index_video_tables(d + off, (int)sz);
                break;   // video track adopted
            }
        }
        off += (int)sz;
    }
}

// Scan mdat for concatenated JPEG streams (FFD8 ... FFD9).  Frame
// offsets are stored as ABSOLUTE file offsets (base_off = mdat payload
// offset within the file) so playback can address vd_data + off.
static void vd_scan_frames(const uint8_t* d, int len, int base_off) {
    vd_frame_count = 0;
    int i = 0;
    while (i + 2 < len && vd_frame_count < VD_MAX_FRAMES) {
        if (d[i] == 0xFF && d[i+1] == 0xD8) {       // SOI
            int start = i;
            int j = i + 2;
            while (j + 1 < len) {
                if (d[j] == 0xFF && d[j+1] == 0xD9) break;  // EOI
                j++;
            }
            if (j + 1 >= len) break;                 // truncated tail
            int end = j + 2;
            vd_frame_off[vd_frame_count] = base_off + start;
            vd_frame_len[vd_frame_count] = end - start;
            vd_frame_count++;
            i = end;
        } else {
            i++;
        }
    }
}

// Read the SOF0/1/2 marker from a JPEG stream to get its pixel size
// (baseline: SOF0=0xC0, SOF1=0xC1, SOF2=0xC2).  Returns 1 on success.
// Used to reject frames that would overflow vd_pixbuf before decoding.
static int vd_jpeg_size(const uint8_t* d, int len, int* w, int* h) {
    int i = 2;   // skip SOI
    while (i + 4 <= len) {
        if (d[i] != 0xFF) { i++; continue; }
        int m = d[i+1];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            // SOF segment layout (marker at i+1):
            //   FF C0 LEN_hi LEN_lo PREC H_hi H_lo W_hi W_lo ...
            // so height lives at i+5..6 and width at i+7..8 — NOT
            // i+3..4 (that would be LEN_lo/PREC and yield garbage).
            if (i + 9 > len) return 0;
            *h = (d[i+5] << 8) | d[i+6];
            *w = (d[i+7] << 8) | d[i+8];
            return 1;
        }
        if (m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            i += 2;                                  // no length field
            continue;
        }
        if (i + 4 > len) return 0;
        int seglen = (d[i+2] << 8) | d[i+3];
        if (seglen < 2) return 0;
        i += 2 + seglen;
    }
    return 0;
}

// ─── Public API ─────────────────────────────────────────────────
void video_init(const uint8_t* data, int len, const char* name) {
    vd_active = 0;
    vd_mjpeg  = 0;
    vd_frame_count = 0;
    vd_frame = 0;
    vd_play_ms = 0;
    vd_wall_base_us = timer_us();   // anchor the master TSC clock at 0
    vd_playing = 1;
    vd_dropped = 0;
    vd_decoded_frame = -1;
    vd_blitted_frame = -1;
    vd_front_idx = 0;
    vd_back_frame = -2;
    vd_back_ok = 0;
    vd_back_w = vd_back_h = 0;
    vd_last_vx = vd_last_vy = vd_last_vw = vd_last_vh = 0;
    vd_decode_ok = 0;
    vd_dec_w = vd_dec_h = 0;
    vd_eco = 0;
    vd_eco_slow = vd_eco_fast = 0;
    vd_width = vd_height = 0;
    vd_dur_ms = 0;
    vd_meta_frames = 0;
    vd_fps = 12;
    vd_brand[0] = vd_codec[0] = 0;
    vd_mp3_len = 0;
    vd_data = data;
    vd_len = len;
    if (name) {
        int i = 0;
        while (name[i] && i < 62) { vd_title[i] = name[i]; i++; }
        vd_title[i] = 0;
    } else vd_title[0] = 0;
    if (len < 16) return;

    // ftyp — major brand at payload start
    if (vd_fcc(data + 4, "ftyp")) {
        for (int i = 0; i < 4; i++) vd_brand[i] = data[8 + i];
        vd_brand[4] = 0;
    }

    // moov → walk traks for the video track (tkhd/mdhd/stsd).
    // vd_find_box outputs offsets RELATIVE TO data, so the payload
    // is `data + *po` — never the returned pointer + offset.
    int mo, ms;
    if (vd_find_box(data, len, "moov", &mo, &ms)) {
        vd_parse_traks(data + mo, ms);
        // Also assemble the in-container MP3 audio track (the video
        // opener will attach it unless a .pcm sidecar is preferred).
        vd_extract_mp3(data + mo, ms);
    }

    // mdat → JPEG frame scan — FALLBACK ONLY.  The preferred index
    // is built from the video track's sample tables in
    // vd_parse_traks/vd_index_video_tables (exact sample boundaries);
    // the byte scan below is kept for containers whose moov lacks
    // usable tables.  It must NOT be the primary path: the MP3 audio
    // track shares mdat, and stray FF D8/FF D9 pairs inside the
    // Huffman-coded audio register as fake frames, inflating the
    // count and corrupting the PTS math.  base_off is the mdat
    // payload offset relative to data so the frame index holds
    // ABSOLUTE file offsets (vd_data + off addresses them).
    if (vd_frame_count == 0) {
        int xo, xs;
        if (vd_find_box(data, len, "mdat", &xo, &xs) && vd_mjpeg)
            vd_scan_frames(data + xo, xs, xo);
    }

    // Refine fps from duration/frame-count when both are known
    if (vd_frame_count > 1 && vd_dur_ms > 0) {
        int f = (int)((uint64_t)vd_frame_count * 1000 / vd_dur_ms);
        if (f >= 1 && f <= 60) vd_fps = f;
    }

    vd_trace("VDX idx="); vd_trace_num((uint32_t)vd_frame_count);
    vd_trace(" meta=");   vd_trace_num((uint32_t)vd_meta_frames);
    vd_trace(" dur=");    vd_trace_num((uint32_t)vd_dur_ms);
    vd_trace(" fps=");    vd_trace_num((uint32_t)vd_fps);
    vd_trace("\n");

    // A parse that produced a decodable picture activates the player:
    // every vd_* consumer (video_tick, the pump, the blitter) gates on
    // vd_active, and this is the ONLY place it may turn on.  Videos
    // without a frame index stay inactive so the desktop just shows
    // the file, not a dead player.
    if (vd_mjpeg && vd_frame_count > 0)
        vd_active = 1;

    // Detach any stale audio sidecar — the opener calls
    // video_set_audio() right after if this video has one.
    vd_audio = NULL; vd_audio_len = 0; vd_audio_rate = 0;
    audio_begin(0, 0, 0, 0);          // clears audio.c state fully
}

void video_set_audio(const uint8_t* pcm, int len, int rate) {
    vd_audio = pcm;
    vd_audio_len = len;
    vd_audio_rate = rate;
    // Load into the speaker engine immediately — audio_begin also
    // populates the state audio_has_audio()/audio_is_playing() read.
    audio_begin(pcm, len, rate, 0);
}

// Attach the MP3 elementary stream extracted from the container.
// Preferred over the .pcm sidecar when present (real soundtrack,
// decoded in the background task instead of the 6 kHz sidecar).
void video_set_audio_mp3(void) {
    if (vd_mp3_len <= 0) return;
    audio_begin_mp3(vd_mp3_buf, vd_mp3_len, 0);
}

// 1 if this video carries an MP3 audio track in its container.
int video_has_mp3(void) {
    return vd_mp3_len > 0;
}

// Called when the player is closed (ESC or the [X] button).  Stops
// the sound immediately and detaches the sidecar so audio can't keep
// playing after the video is gone.
void video_close(void) {
    audio_begin(0, 0, 0, 0);
    audio_stop_mp3();
    vd_audio = NULL; vd_audio_len = 0; vd_audio_rate = 0;
    vd_mp3_len = 0;
    vd_active = 0;
}

// Keep the audio in step with the video transport state.  Runs on
// play/pause/seek; while playing it nudges the read position to the
// current frame time so the sound tracks the transport.  Works for
// both the .pcm sidecar and the in-container MP3 stream.
//
// Also re-anchors the master TSC clock: whenever the playhead is
// explicitly placed (seek, resume, loop), the free-run wall clock is
// reset so the audio/master PTS timeline stays consistent.
static void vd_sync_audio(void) {
    vd_wall_base_us = timer_us();
    if (!audio_has_audio()) return;
    int cur_ms = 0;
    if (vd_frame_count > 1 && vd_dur_ms > 0)
        cur_ms = (int)((uint64_t)vd_frame * vd_dur_ms / vd_frame_count);
    if (!vd_playing) { audio_stop(); return; }
    if (audio_is_playing()) {
        audio_set_pos_ms(cur_ms);   // routes to the MP3 worker when active
        return;
    }
    if (vd_mp3_len > 0) { audio_begin_mp3(vd_mp3_buf, vd_mp3_len, cur_ms); return; }
    audio_begin(vd_audio, vd_audio_len, vd_audio_rate, cur_ms);
}

// ─── Sub-tick decode timing (PIT channel 0 countdown) ────────────
// The PIT runs at 1.193182 MHz with reload value 23863 (see
// irq_install) — one tick ≈ 20 ms.  Channel 0 is programmed in mode
// 3 (square wave), where the count register decrements by TWO per
// clock, so a full period is DIV register steps and the fractional
// term must be doubled when converting to clock counts.  Combined
// with the tick counter this gives ~ms-resolution decode timing for
// the ECO state machine (bounded error ≈ one period, which is fine
// against the 125 ms frame budget at 8 fps).
#define VD_PIT_DIV  23863
#define VD_PIT_FREQ 1193182

static uint32_t vd_pit_count(void) {
    outb(0x43, 0x00);              // latch channel 0
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint32_t)((hi << 8) | lo);
}

// Milliseconds elapsed since (t0, c0) was sampled with tick+
// vd_pit_count().  Whole periods come from the tick delta; the
// fractional part is (since1 - since0)*2 because mode 3 steps the
// count register by two per clock.  If the countdown reloads before
// the tick IRQ increments (a hardware-vs-software race), the
// fractional term goes negative and is clamped — that hides at most
// one ~20 ms period, negligible at the 125 ms decision scale.
static int vd_ms_since(uint32_t t0, uint32_t c0) {
    uint32_t t1 = tick;
    uint32_t c1 = vd_pit_count();
    // Clamp the latched countdown to the reload divisor: right after
    // a channel-0 divisor load the count can briefly read higher
    // than DIV, and DIV - count would underflow into a bogus value.
    uint32_t r0 = (c0 & 0xFFFF); if (r0 > VD_PIT_DIV) r0 = VD_PIT_DIV;
    uint32_t r1 = (c1 & 0xFFFF); if (r1 > VD_PIT_DIV) r1 = VD_PIT_DIV;
    uint32_t since0 = VD_PIT_DIV - r0;
    uint32_t since1 = VD_PIT_DIV - r1;
    // CRITICAL: cast BEFORE subtracting.  since is a uint32, so
    // `(int64_t)(since1 - since0)` computes the difference in UNSIGNED
    // arithmetic first — and whenever the countdown reloads between
    // the two samples (every 20 ms period), since1 < since0 wraps to
    // ~2^32, then *2 ≈ 8.59e9 counts → a bogus 7,199,321 ms reading
    // that would spuriously flip the ECO ladder.  Casting each operand
    // first keeps the signed difference the tick compensation expects.
    int64_t counts = (int64_t)(t1 - t0) * VD_PIT_DIV
                   + ((int64_t)since1 - (int64_t)since0) * 2;
    if (counts < 0) counts = 0;   // reload race — see comment above
    return (int)(counts * 1000 / VD_PIT_FREQ);
}

// Frame interval in ms for the current video (min 1).  Shared by the
// audio-master sync windows (video_tick / vd_count_dropped) and the
// ECO decode budget so the three frame-rate decisions can never
// disagree about what one frame costs.
static inline int vd_frame_ms(void) {
    int ms = vd_fps > 0 ? 1000 / vd_fps : 20;
    return ms < 1 ? 1 : ms;
}

// Core decoder: decode `frame` into dst (stride 0 = tightly packed
// RGBA) at the current ECO rung, with the decode timer + adaptive
// ladder.  Sets *ow/*oh on success.  Returns 1 = decoded, 2 = decoded
// but the ECO ladder changed rung (caller may re-decode once at the
// new rung), 0 = failed or out of range.  Does NOT touch the
// front/back bookkeeping — callers assign the descriptors.
static int vd_decode_frame(int frame, uint8_t* dst, int stride,
                           int* ow, int* oh) {
    if (!vd_mjpeg || frame < 0 || frame >= vd_frame_count) return 0;
    *ow = *oh = 0;

    // SOF size guard — reject frames that would overflow a buffer.
    // FAIL-SAFE: if the SOF marker can't be parsed (corrupt or
    // truncated JPEG), reject the frame rather than skipping the
    // check — a frame with unparseable headers is not safe to
    // decode on a single-CPU system with interrupts enabled.
    int fw = 0, fh = 0;
    if (!vd_jpeg_size(vd_data + vd_frame_off[frame],
                      vd_frame_len[frame], &fw, &fh))
        return 0;   // SOF parse failed — reject, don't skip
    if (fw > VD_MAX_W || fh > VD_MAX_H ||
        fw * fh * 4 > (int)sizeof(vd_pixbuf[0]))
        return 0;

    JpegImage jim = {0};
    jim.pixels = dst;
    jim.stride = stride;

    // Time the decode; pick the decoder for the current ECO rung.
    // DISABLE INTERRUPTS during decode to prevent PIT IRQ from
    // corrupting the bit reader (bench code confirmed this).
    // RETRY ONCE: if corruption still happens (very rare), a fresh
    // decoder state on the same valid JPEG data succeeds on the
    // second try — two consecutive failures indicate real data corruption.
    uint32_t t0 = tick;
    uint32_t c0 = vd_pit_count();
    int ok;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1) { jim.width = jim.height = 0; }  // reset state
        asm volatile("cli");   // disable interrupts during decode
        if (vd_eco == 0)
            ok = jpeg_decode(vd_data + vd_frame_off[frame],
                             vd_frame_len[frame], &jim);
        else if (vd_eco == 1)
            ok = jpeg_decode_eco_v(vd_data + vd_frame_off[frame],
                                   vd_frame_len[frame], &jim);
        else
            ok = jpeg_decode_eco(vd_data + vd_frame_off[frame],
                                 vd_frame_len[frame], &jim);
        asm volatile("sti");   // re-enable interrupts
        if (ok && jim.width > 0) break;   // success
    }
    if (!ok || jim.width <= 0) return 0;
    *ow = jim.width;
    *oh = jim.height;

    // ── Light deblock ─────────────────────────────
    // Smooth the 8×8 MCU grid right after decode.  Skip for ECO
    // modes (1,2) — already half/quarter resolution, deblocking
    // adds blur and costs significant CPU time.
    if (vd_eco == 0)
        jpeg_deblock_rgba(dst, *ow, *oh, stride);

    int dec_ms = vd_ms_since(t0, c0);   // includes the deblock cost

    // ── Adaptive ECO ladder hysteresis ───────────────────
    // Compare measured decode time against the frame interval:
    // descend at the interval itself (a host over budget steps
    // down; fast hosts are ~5× under so they never false-descend),
    // ascend at ½× the interval (current rung fast enough that the
    // next rung up — ≈2× the cost — would fit).  The ½..1 dead band
    // is the hysteresis that prevents oscillation.
    int budget = vd_frame_ms();
    int rung_changed = 0;
    if (dec_ms >= budget) {
        vd_eco_slow++;
        vd_eco_fast = 0;
        if (vd_eco < 2 && vd_eco_slow >= 2) {
            vd_eco++;
            vd_eco_slow = vd_eco_fast = 0;
            rung_changed = 1;
        }
    } else if (dec_ms <= budget / 2) {
        vd_eco_fast++;
        vd_eco_slow = 0;
        if (vd_eco > 0 && vd_eco_fast >= 4) {
            vd_eco--;
            vd_eco_slow = vd_eco_fast = 0;
            rung_changed = 1;
        }
    } else {
        vd_eco_slow = vd_eco_fast = 0;   // dead band: keep current rung
    }
    return rung_changed ? 2 : 1;
}

// Decode the playhead frame into the FRONT buffer (the one whose
// descriptors vd_decoded_frame etc. describe).  Snapshot the playhead
// ONCE: video_tick() runs in the PIT IRQ and may advance vd_frame
// during the decode, so reading vd_frame again after the JPEG call
// would record a DIFFERENT frame than the pixels actually decoded —
// corrupting the blitted/decoded bookkeeping that the audio-master
// drop counter relies on.
static void vd_decode_current(uint8_t* dst, int stride) {
    if (!vd_mjpeg || vd_frame < 0 || vd_frame >= vd_frame_count) {
        vd_decoded_frame = -1;
        vd_decode_ok = 0;
        vd_dec_w = vd_dec_h = 0;
        return;
    }
    if (vd_decoded_frame == vd_frame) return;   // already cached (incl. failures)

    int target = vd_frame;
    int w = 0, h = 0;
    int r = vd_decode_frame(target, dst, stride, &w, &h);
    if (r == 2)   // ladder moved mid-decode — one re-decode at the new rung
        r = vd_decode_frame(target, dst, stride, &w, &h);
    if (r >= 1) {
        vd_dec_w = w;
        vd_dec_h = h;
        vd_decode_ok = 1;
        vd_decoded_frame = target;
    } else {
        vd_decoded_frame = target;             // mark visited, no retry
        vd_decode_ok = 0;
        vd_dec_w = vd_dec_h = 0;
    }
}

// ─── Decode-ahead (double buffering) ─────────────────────────────
// After the present, decode the frame the playhead will show NEXT
// into the back buffer during the current frame's window, so the
// next present is a pure blit at its PTS — no decode in the present
// path.  Best-effort: if a decode runs long the ECO ladder descends
// (bounded cost) and the synchronous present path catches up; if the
// audio-master clock drops frames, the next call simply re-targets
// the new playhead.  Runs in MAIN-LOOP context only
// (desktop_video_pump), never inside the PIT IRQ.
static void vd_decode_ahead(void) {
    if (!vd_active || !vd_mjpeg || !vd_playing || vd_frame_count <= 1)
        return;
    // Frame we'll present next: the playhead if it isn't on screen
    // yet (audio-master may have jumped ahead of the blit), else the
    // frame after the one being shown.
    int target;
    if (vd_frame > vd_blitted_frame) target = vd_frame;
    else target = vd_frame + 1;
    if (target < 0 || target >= vd_frame_count) return;
    if (vd_back_frame == target && vd_back_ok) return;   // already decoded
    uint8_t* dst = vd_pixbuf[1 - vd_front_idx];
    int w = 0, h = 0;
    int r = vd_decode_frame(target, dst, 0, &w, &h);
    if (r == 2)   // ladder moved — one re-decode at the new rung
        r = vd_decode_frame(target, dst, 0, &w, &h);
    if (r >= 1) {
        vd_back_frame = target;
        vd_back_w = w;
        vd_back_h = h;
        vd_back_ok = 1;
    }
}

// Called from desktop_video_pump (main loop) right after the frame
// redraw, so the next frame is pre-decoded for its PTS.
void video_pump_ahead(void) {
    vd_decode_ahead();
}

// Count frames that were never shown because decode fell behind the
// audio clock: every frame strictly between the last PRESENTED frame
// and the frame just decoded was skipped (MJPEG frames are all
// non-reference — independently decodable — so a skipped frame is
// gone forever and can never corrupt the stream).  prev == -1 means
// nothing has been shown yet (first frame, or a user seek), so
// nothing is counted.  Runs in main-loop context (video_draw is only
// reached from the pump / normal draw, never from the PIT IRQ).
static inline void vd_count_dropped(int prev, int shown) {
    if (prev < 0 || shown <= prev + 1) return;
    int skipped = shown - prev - 1;
    // Drop window in wall-clock time: only skips whose TOTAL duration
    // reaches VD_DROP_DEAD_MS are worth reporting.  A single 41 ms
    // frame at 24 fps is sub-threshold jitter, while a 2-frame skip
    // at 8 fps (250 ms) clearly counts — keeping the DROP counter
    // comparable across videos with very different frame rates.  At
    // the default 8 fps transcode this is identical to before (any
    // 1-frame skip is 125 ms and counts).
    if (skipped * vd_frame_ms() >= VD_DROP_DEAD_MS)
        vd_dropped += skipped;
}

// ─── Draw ────────────────────────────────────────────────────────
#define VD_TB_H   28          // title bar height
#define VD_TR_H   46          // transport bar height
#define VD_ST_H   18          // status line height

static void vd_win_geom(int* wx, int* wy, int* ww, int* wh,
                        int* ty, int* btn_x, int* btn_s,
                        int* sb_x, int* sb_y, int* sb_w) {
    int W = (int)vesa.width, H = (int)vesa.height;
    int w = W - 140; if (w > 760) w = 760; if (w < 360) w = 360;
    int vh = w * 9 / 16; if (vh > H - 170) vh = H - 170; if (vh < 140) vh = 140;
    int hh = VD_TB_H + vh + VD_TR_H + VD_ST_H;
    int x = (W - w) / 2, y = (H - hh) / 2;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    *wx = x; *wy = y; *ww = w; *wh = hh;
    *ty = y + VD_TB_H + vh + 8;
    *btn_x = x + 14; *btn_s = 30;
    *sb_x = *btn_x + *btn_s + 14;
    *sb_y = *ty + *btn_s/2 - 3;
    *sb_w = x + w - 16 - *sb_x;
}

static void vd_time_str(int ms, char* out) {
    int s = ms / 1000, m = s / 60; s %= 60;
    out[0] = '0' + m / 10; out[1] = '0' + m % 10; out[2] = ':';
    out[3] = '0' + s / 10; out[4] = '0' + s % 10; out[5] = 0;
}

static void vd_num(char* st, int* si, int v) {
    if (v > 0) {
        char tmp[8]; int ti = 0;
        while (v > 0) { tmp[ti++] = '0' + v % 10; v /= 10; }
        while (ti > 0) st[(*si)++] = tmp[--ti];
    } else st[(*si)++] = '?';
}

void video_draw(void) {
    if (!vd_active) return;
    int wx, wy, ww, wh, ty, btn_x, btn_s, sb_x, sb_y, sb_w;
    vd_win_geom(&wx, &wy, &ww, &wh, &ty, &btn_x, &btn_s,
                &sb_x, &sb_y, &sb_w);
    int vh = wh - VD_TB_H - VD_TR_H - VD_ST_H;

    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;

    // ── Title bar ──────────────────────────────────────────────
    for (int y = 0; y < VD_TB_H; y++) {
        int t = y * 255 / VD_TB_H;
        int r = 0x14 + (0x08 - 0x14) * t / 255;
        int g = 0x12 + (0x06 - 0x12) * t / 255;
        int b = 0x30 + (0x22 - 0x30) * t / 255;
        vesa_hline(wx, wy + y, ww, (r << 16) | (g << 8) | b);
    }
    vesa_hline(wx, wy, ww, 0x6A4FBF);
    vesa_hline(wx, wy + VD_TB_H - 1, ww, 0x060310);
    vesa_draw_string(wx + 10, wy + (VD_TB_H - 8) / 2, vd_title[0] ? vd_title : "Video",
                     0xE8D8FF, 0);
    draw_close_btn(wx + ww - 32, wy + (VD_TB_H - 16) / 2, 0x0E0A1E, "Video");

    // Video area
    int vx = wx + 6, vy = wy + VD_TB_H, vw = ww - 12, vh2 = vh;

    if (vd_mjpeg && vd_frame_count > 0) {
        // ── Zero-copy dirty guard ─────────────────────────────
        // The draw loop runs at heartbeat rate (~18 Hz) but frames
        // change at the video's fps (~8 Hz).  Re-decoding and
        // re-blitting the SAME pixels every tick is pure memory
        // traffic, so skip the whole video area when the current
        // frame is already on screen, no re-decode is pending, and
        // the window hasn't moved.  (Transport/status bars below
        // still redraw every tick.)
        int geom_changed = vx != vd_last_vx || vy != vd_last_vy ||
                           vw != vd_last_vw || vh2 != vd_last_vh;
        vd_last_vx = vx; vd_last_vy = vy;
        vd_last_vw = vw; vd_last_vh = vh2;

        if (geom_changed || vd_blitted_frame != vd_frame ||
            vd_decoded_frame != vd_frame) {
            // ── PTS gate ────────────────────────────────────
            // Compare the frame's presentation timestamp against the
            // master clock BEFORE showing it.  video_tick() already
            // advances vd_frame only when the master clock crosses
            // the frame's PTS, so this normally passes instantly — it
            // is a safety net for a heartbeat racing the IRQ: never
            // present a frame whose time hasn't arrived (hold the
            // previous frame's pixels; the next heartbeat re-checks).
            if (!geom_changed && vd_blitted_frame != vd_frame &&
                vd_master_us() < vd_frame_pts_us(vd_frame)) {
                // early — hold; nothing to draw this tick
            } else {
                vesa_fill_rect(vx, vy, vw, vh2, 0x000000);
                vesa_draw_rect(vx, vy, vw, vh2, 0x2A2060);
                int prev_blit = vd_blitted_frame;

                // ── Promote the decode-ahead back buffer ────
                // If the back buffer already holds the frame the
                // playhead points at, promote it (flip the front
                // index — zero data movement) and present with a
                // pure blit — no decode in the present path, so the
                // frame lands at its PTS instead of a decode later.
                if (vd_back_frame == vd_frame && vd_back_ok) {
                    vd_front_idx = 1 - vd_front_idx;
                    vd_decoded_frame = vd_back_frame;
                    vd_dec_w = vd_back_w;
                    vd_dec_h = vd_back_h;
                    vd_decode_ok = vd_back_ok;
                    vd_back_frame = -2;
                    vd_back_ok = 0;
                } else if (!(vd_decoded_frame == vd_frame && vd_decode_ok)) {
                    // Not pre-decoded — synchronous fallback decode
                    // into the front buffer (first frame, a seek, or
                    // a drop the decode-ahead didn't catch).
                    vd_decode_current(vd_pixbuf[vd_front_idx], 0);
                }

                int shown = vd_decoded_frame;   // frame whose pixels are ready
                if (vd_decode_ok && vd_dec_w > 0 && vd_dec_h > 0) {
                    // Letterbox-scale the frame into the video area
                    int sw = vd_dec_w, sh = vd_dec_h;
                    int dw = vw; int dh = sh * vw / sw;
                    if (dh > vh2) { dh = vh2; dw = sw * vh2 / sh; }
                    int dx = vx + (vw - dw) / 2;
                    int dy = vy + (vh2 - dh) / 2;
                    // Exact fit (native size) is a plain RGBA blit —
                    // one row-wise copy, no filter.  Scaled frames use
                    // Catmull-Rom bicubic at full res (4-tap separable,
                    // sharper than bilinear); on the ECO rungs the
                    // blit is dest-bound at ~4× bilinear cost, so drop
                    // to bilinear there so slow hosts never overshoot
                    // the frame interval.
                    if (dw == sw && dh == sh)
                        vesa_blit_rgba(dx, dy, vd_pixbuf[vd_front_idx],
                                       sw, sh, 0, 0, sw, sh);
                    else if (vd_eco == 0)
                        vesa_blit_rgba_scaled_bicubic(dx, dy, dw, dh,
                                                      vd_pixbuf[vd_front_idx], sw, sh);
                    else
                        vesa_blit_rgba_scaled_bilinear(dx, dy, dw, dh,
                                                       vd_pixbuf[vd_front_idx], sw, sh);
                    vd_blitted_frame = shown;
                    vd_count_dropped(prev_blit, shown);
                } else {
                    // Keep the error text on screen but stop re-filling
                    // the area every tick (the draw loop outruns the
                    // video fps by ~2x).  The next frame re-renders.
                    vd_blitted_frame = shown;
                    vesa_draw_string(vx + vw/2 - 70, vy + vh2/2 - 4,
                                     "Frame decode failed", 0x8866AA, 0);
                }
            }
        }
        // else: video area unchanged — the pixels already on screen
        // are left alone (zero memory traffic this tick).
    } else {
        // Unsupported codec — show the actual fourcc so the user knows why.
        // (avc1 = H.264, hvc1/hev1 = H.265, mp4v = MPEG-4 ...)
        vesa_fill_rect(vx, vy, vw, vh2, 0x000000);
        vesa_draw_rect(vx, vy, vw, vh2, 0x2A2060);
        char cmsg[40];
        int ci = 0;
        const char* pre = "Codec ";
        while (pre[ci]) { cmsg[ci] = pre[ci]; ci++; }
        if (vd_codec[0]) {
            for (int q = 0; q < 4 && vd_codec[q]; q++) cmsg[ci++] = vd_codec[q];
        } else {
            cmsg[ci++] = '?'; cmsg[ci++] = '?'; cmsg[ci++] = '?'; cmsg[ci++] = '?';
        }
        cmsg[ci] = 0;
        int cl = 0; while (cmsg[cl]) cl++;
        vesa_draw_string(vx + vw/2 - cl*4, vy + vh2/2 - 12, cmsg, 0x8866AA, 0);
        vesa_draw_string(vx + vw/2 - 90, vy + vh2/2 + 6,
                         "Only MJPEG is supported", 0x6655AA, 0);
    }

    // ── Transport bar ──────────────────────────────────────────
    int by = ty;
    // Play/pause button (rounded square)
    vesa_fill_rect(btn_x, by, btn_s, btn_s, 0x241A4A);
    vesa_draw_rect(btn_x, by, btn_s, btn_s, 0x6A4FBF);
    if (vd_playing) {
        // pause: two bars
        int bw = btn_s / 5, bh = btn_s * 2 / 5;
        int bx = btn_x + btn_s/2 - bw - 2, bx2 = by + btn_s/2 - bh/2;
        vesa_fill_rect(bx, bx2, bw, bh, 0xE8D8FF);
        vesa_fill_rect(bx + bw + 4, bx2, bw, bh, 0xE8D8FF);
    } else {
        // play: right-pointing triangle
        int cx = btn_x + btn_s/2 + 1, cy = by + btn_s/2;
        int r0 = btn_s / 4;
        for (int rr = -r0; rr <= r0; rr++) {
            int hw = (r0 - (rr < 0 ? -rr : rr)) * 3 / 4 + 1;
            vesa_hline(cx, cy + rr, hw, 0xE8D8FF);
        }
    }

    // Seek bar
    int cur_ms = 0, tot_ms = vd_dur_ms;
    if (vd_frame_count > 1)
        cur_ms = (int)((uint64_t)vd_frame * vd_dur_ms / vd_frame_count);
    vesa_fill_rect(sb_x, sb_y, sb_w, 6, 0x14102A);
    int frac = vd_frame_count > 1 ? vd_frame * sb_w / vd_frame_count : 0;
    if (frac > 0) {
        vesa_fill_rect(sb_x, sb_y, frac, 6, 0x8A6FDB);
        vesa_fill_rect(sb_x + frac - 2, sb_y - 2, 4, 10, 0xC0A8FF);
    }

    char tb[16], tb2[16];
    vd_time_str(cur_ms, tb);
    vd_time_str(tot_ms, tb2);
    vesa_draw_string(sb_x + 4, ty + btn_s + 2, tb, 0x8899BB, 0);
    int tl = 0; while (tb2[tl]) tl++;
    vesa_draw_string(sb_x + sb_w - tl * 8 - 4, ty + btn_s + 2, tb2, 0x8899BB, 0);

    // ── Status line ────────────────────────────────────────────
    char st[96];
    int si = 0;
    if (vd_mjpeg && vd_frame_count > 0) {
        const char* f1 = "MJPEG";
        while (f1[si]) { st[si] = f1[si]; si++; }
    } else {
        const char* f2 = "Unsupported";
        while (f2[si]) { st[si] = f2[si]; si++; }
    }
    st[si++] = ' '; st[si++] = '|'; st[si++] = ' ';
    vd_num(st, &si, vd_width);
    st[si++] = 'x';
    vd_num(st, &si, vd_height);
    st[si++] = ' '; st[si++] = '|'; st[si++] = ' ';
    vd_num(st, &si, vd_fps);
    st[si++] = ' '; st[si++] = 'f'; st[si++] = 'p'; st[si++] = 's';
    st[si++] = ' '; st[si++] = '|'; st[si++] = ' ';
    if (vd_brand[0]) { const char* b = vd_brand; while (*b && si < 90) st[si++] = *b++; }
    else st[si++] = '?';
    st[si++] = ' '; st[si++] = '|'; st[si++] = ' ';
    vd_num(st, &si, vd_frame_count > 0 ? vd_frame_count : vd_meta_frames);
    st[si++] = ' '; st[si++] = 'f'; st[si++] = 'r'; st[si++] = 'a'; st[si++] = 'm'; st[si++] = 'e'; st[si++] = 's';
    // Audio state — shows MUTE or the current volume level (VOL nn),
    // plus MONO when the stereo pair is downmixed.  Volume follows
    // the kernel-wide sound_volume knob; [-]/[+] adjust it in
    // video_handle_key (gliding, thanks to audio.c's smoothed gain).
    if (audio_has_audio()) {
        st[si++] = ' '; st[si++] = '|'; st[si++] = ' ';
        if (audio_is_muted()) {
            const char* mt = "MUTE";
            while (*mt && si < 88) st[si++] = *mt++;
        } else {
            const char* vl = "VOL";
            while (*vl && si < 88) st[si++] = *vl++;
            st[si++] = ' ';
            vd_num(st, &si, audio_get_volume());
        }
        if (audio_is_mono()) {
            st[si++] = ' ';
            const char* mn = "MONO";
            while (*mn && si < 88) st[si++] = *mn++;
        }
    }
    // ECO indicator — which reduced-res rung is active, shown as
    // the ACTUAL decoded dims (ECO 576x288 = vertical half, keeps
    // full width; ECO 288x288 = both axes dropped).
    if (vd_eco) {
        st[si++] = ' '; st[si++] = '|'; st[si++] = ' ';
        const char* ec = "ECO";
        while (*ec && si < 90) st[si++] = *ec++;
        st[si++] = ' ';
        if (vd_eco == 1) {
            vd_num(st, &si, vd_dec_w > 0 ? vd_dec_w : vd_width);
            st[si++] = 'x';
            vd_num(st, &si, vd_dec_h > 0 ? vd_dec_h : vd_height / 2);
        } else {
            vd_num(st, &si, vd_dec_w > 0 ? vd_dec_w : vd_width / 2);
            st[si++] = 'x';
            vd_num(st, &si, vd_dec_h > 0 ? vd_dec_h : vd_height / 2);
        }
    }
    // Dropped-frame counter — non-reference frames skipped to keep
    // the picture in step with the audio clock (see the audio-master
    // block in video_tick).  0 is normal on a fast host; it only
    // grows when decode falls behind the sound.  vd_num() is
    // unbounded, so cap the value (and the room left in st) before
    // writing — DROP is the last field in the 96-byte status buffer.
    if (vd_dropped > 0 && si < 80) {
        st[si++] = ' '; st[si++] = '|'; st[si++] = ' ';
        const char* dr = "DROP";
        while (*dr && si < 88) st[si++] = *dr++;
        st[si++] = ' ';
        int dc = vd_dropped > 9999 ? 9999 : vd_dropped;   // 4-digit cap
        vd_num(st, &si, dc);
    }
    st[si] = 0;
    vesa_draw_string(wx + 10, wy + wh - VD_ST_H, st, 0x7788AA, 0);
}

// ─── Keyboard ───────────────────────────────────────────────────
void video_handle_key(char c) {
    if (!vd_active) return;
    if (c == ' ' || c == 'p' || c == 'P') {         // play/pause
        vd_playing = !vd_playing;
        vd_sync_audio();
        return;
    }
    if (c == 'm' || c == 'M') {                     // mute toggle
        if (audio_has_audio()) audio_mute(!audio_is_muted());
        return;
    }
    if (c == '-') {                                 // volume down (glides)
        if (audio_has_audio()) audio_set_volume(audio_get_volume() - 5);
        return;
    }
    if (c == '+' || c == '=') {                     // volume up (glides)
        if (audio_has_audio()) audio_set_volume(audio_get_volume() + 5);
        return;
    }
    if (c == 'n' || c == 'N') {                     // mono/stereo mix toggle
        audio_set_mono(!audio_is_mono());
        return;
    }
    if (c == 0x4B) {                                // Left = seek -1s
        if (vd_mjpeg && vd_frame_count > 0) {
            int step = vd_fps > 0 ? vd_fps : 12;
            vd_frame -= step;
            if (vd_frame < 0) vd_frame = 0;
            vd_play_ms = 0;
            vd_blitted_frame = -1;   // a seek is a jump, not a drop
            vd_sync_audio();
        }
        return;
    }
    if (c == 0x4D) {                                // Right = seek +1s
        if (vd_mjpeg && vd_frame_count > 0) {
            int step = vd_fps > 0 ? vd_fps : 12;
            vd_frame += step;
            if (vd_frame >= vd_frame_count) vd_frame = vd_frame_count - 1;
            vd_play_ms = 0;
            vd_blitted_frame = -1;   // a seek is a jump, not a drop
            vd_sync_audio();
        }
        return;
    }
}

// ─── Mouse ──────────────────────────────────────────────────────
int video_handle_mouse(int clk_x, int clk_y) {
    if (!vd_active) return 0;
    int wx, wy, ww, wh, ty, btn_x, btn_s, sb_x, sb_y, sb_w;
    vd_win_geom(&wx, &wy, &ww, &wh, &ty, &btn_x, &btn_s,
                &sb_x, &sb_y, &sb_w);

    // Play/pause button
    if (clk_x >= btn_x && clk_x < btn_x + btn_s &&
        clk_y >= ty && clk_y < ty + btn_s) {
        vd_playing = !vd_playing;
        vd_sync_audio();
        return 1;
    }

    // Seek bar
    if (vd_mjpeg && vd_frame_count > 0 &&
        clk_x >= sb_x && clk_x < sb_x + sb_w &&
        clk_y >= sb_y - 6 && clk_y < sb_y + 14) {
        int nx = clk_x - sb_x;
        if (nx < 0) nx = 0;
        if (nx > sb_w) nx = sb_w;
        vd_frame = nx * vd_frame_count / sb_w;
        if (vd_frame >= vd_frame_count) vd_frame = vd_frame_count - 1;
        vd_play_ms = 0;
        vd_blitted_frame = -1;   // a seek is a jump, not a drop
        vd_sync_audio();
        return 1;
    }
    return 0;
}

// ─── Heartbeat playback advance (PIT ~18Hz) ─────────────────────
// Returns 1 when the frame advanced and the desktop should redraw
// (the heartbeat calls desktop_redraw_no_click() for us, since that
// helper is private to desktop.c).
// ─── Heartbeat playback advance (PIT ~50 Hz) ─────────────────────
// The PIT is reprogrammed to ~50 Hz (divisor 23863), so one tick ≈
// 20 ms.  BUT desktop_heartbeat() is called from both the IRQ0
// handler AND the main loop — so video_tick() fires twice per real
// 20 ms tick.  Guard with a tick-counter so the clock advances only
// once per real tick, preventing the ~5.5× playback speed bug.
//
// The global tick counter is declared at the top of this file (next
// to the sub-tick timing helper); video_tick() reads it here.
//
// Returns 1 when the frame advanced and the desktop should redraw
// (the desktop.c heartbeat sets video_redraw_pending, and the actual
// decode+draw runs in desktop_video_pump() in main-loop context).
static uint32_t vd_last_tick = 0;

int video_tick(void) {
    // Master-clock probe (E2E hook, like VDX): every ~250 ms while a
    // video is open, dump vd_active, the master clock (µs), the
    // playhead frame, audio-present and playing flags.  This is what
    // proved the frozen-playhead bug — m= advanced in real time while
    // f= stayed 0 because vd_active was still 0 (no setter existed).
    static uint32_t vd_dbg_tick = 0;
    if (tick - vd_dbg_tick >= 12) {
        vd_dbg_tick = tick;
        vd_trace("VDV a=");   vd_trace_num((uint32_t)vd_active);
        vd_trace(" m=");    vd_trace_num((uint32_t)vd_master_us());
        vd_trace(" f=");    vd_trace_num((uint32_t)vd_frame);
        vd_trace(" ah=");   vd_trace_num((uint32_t)(audio_has_audio() && audio_is_playing()));
        vd_trace(" pl=");   vd_trace_num((uint32_t)vd_playing);
        vd_trace("\n");
    }
    if (!vd_active || !vd_playing || !vd_mjpeg || vd_frame_count < 2) return 0;
    // Guard: only advance once per real PIT tick.
    if (tick == vd_last_tick) return 0;
    vd_last_tick = tick;

    int frame_ms = vd_frame_ms();

    // ── ms-based A/V sync windows ──────────────────────────────
    // The sync tolerances are defined in wall-clock milliseconds and
    // converted to frames for THIS video's interval, so every video
    // gets proportionally similar windows regardless of its frame
    // rate: a 250 ms hold is 2 frames at 8 fps but 6 frames at
    // 24 fps.  Each window floors at 1 frame.
    int hold_frames = VD_AHEAD_HOLD_MS / frame_ms;
    if (hold_frames < 1) hold_frames = 1;

    // ── Audio-mastered sync ────────────────────────────────────
    // While the PCM sidecar plays, the speaker IRQ advances the
    // audio clock in REAL time no matter how long the main loop
    // spends JPEG-decoding a frame.  The playhead therefore follows
    // the audio position: the frame advances only when the sound
    // crosses into the next frame's time.  If a slow decode lets the
    // audio run ahead, the frames in between are DROPPED and the
    // playhead jumps straight to the audio-synced frame — every
    // MJPEG frame is a non-reference I-frame (independently
    // decodable), so skipping any of them can never corrupt the
    // stream.  If the playhead instead runs AHEAD of the sound, the
    // picture holds (repeats the last frame) until the sound catches
    // up — see the af < vd_frame block below.  Either way the
    // picture stays glued to the sound instead of drifting from it.
    if (audio_has_audio() && audio_is_playing() && vd_dur_ms > 0) {
        // AUDIO is the master clock: the playhead follows the audio
        // block PTS (µs).  af = the frame whose PTS is at/past the
        // audio position — the same µs timeline as vd_frame_pts_us.
        uint64_t ams_us = vd_master_us();
        int af = (int)(ams_us * vd_frame_count / ((uint64_t)vd_dur_ms * 1000));
        if (af >= vd_frame_count) {
            // Audio outlasted the picture — loop both back to the
            // start, mirroring the wall-clock loop below.
            vd_frame = 0;
            vd_play_ms = 0;
            if (audio_has_audio()) audio_set_pos_ms(0);
            vd_sync_audio();
            vd_decoded_frame = -1;
            return 1;
        }
        if (af > vd_frame) {
            // Audio is ahead of the playhead: drop the frames that
            // would have played in between (they were never shown)
            // and land on the frame matching the sound right now.
            // The gap is tallied by vd_count_dropped() at blit time
            // (next pump), so it is never counted here too.
            vd_frame = af;
            vd_play_ms = 0;
            vd_decoded_frame = -1;   // decode the audio-synced frame now
            return 1;
        }
        // ── Video AHEAD of the audio clock ─────────────────────
        // The playhead sits at or past the sound.  At steady state
        // the two are lockstep (the playhead is derived from the
        // audio clock), so this is always transient: a backward
        // seek's ms→sample truncation, a clock hiccup, or an
        // external playhead jump.
        //
        // GENTLE DRIFT CORRECTION (instead of hard hold/jump):
        // When the video is only slightly ahead (1-2 frames), the
        // old code froze playback entirely (return 0 = repeat frame)
        // until audio caught up — causing visible stutter.  Now we
        // use a WEIGHTED BLEND: the effective PTS is 80% video + 20%
        // audio when 1 frame ahead, 60/40 when 2 frames, etc.
        // This gently pulls the playhead toward the audio position
        // over several frames, avoiding both the stutter of a hard
        // hold and the jump of a re-anchor.
        //
        // For larger drifts (>hold_frames) the old hard re-anchor
        // still applies — that's a real desync that needs an
        // immediate correction.
        if (af < vd_frame) {
            int drift = vd_frame - af;
            if (drift > hold_frames) {
                // Large drift — hard re-anchor (can't wait this out)
                vd_frame = af;
                vd_play_ms = 0;
                vd_decoded_frame = -1;
                return 1;
            }
            // Gentle drift correction: weighted blend of video and
            // audio positions.  At drift=1: 80% video / 20% audio
            // (nearly invisible nudge).  At drift=hold_frames: ~50/50
            // (strong pull).  The blend is quantized to frames.
            int blend_pct = 20 + (drift - 1) * 30 / (hold_frames > 1 ? hold_frames - 1 : 1);
            if (blend_pct > 50) blend_pct = 50;
            int blended = (vd_frame * (100 - blend_pct) + af * blend_pct) / 100;
            if (blended < af) blended = af;       // never fall behind audio
            if (blended > vd_frame) blended = vd_frame; // never jump ahead
            if (blended != vd_frame) {
                vd_frame = blended;
                vd_play_ms = 0;
                vd_decoded_frame = -1;
                return 1;   // frame changed — redraw needed
            }
            return 0;   // blended position same as current — hold
        }
        // af == vd_frame: audio mid-frame — hold until it crosses.
        return 0;
    }

    // No audio (or it finished) — free-run on the VIDEO clock.  The
    // TSC µs timer (cpu/timer.c) is master here: the playhead is
    // derived from (now - vd_wall_base_us) so playback advances
    // smoothly at µs resolution instead of in chunky 20 ms PIT steps.
    uint64_t master_us = vd_master_us();
    uint64_t frame_us  = (uint64_t)frame_ms * 1000;
    int target = (int)(master_us / frame_us);
    if (target >= vd_frame_count) {         // loop — restart audio too
        target = 0;
        if (audio_has_audio()) audio_set_pos_ms(0);
        vd_sync_audio();                    // re-anchors vd_wall_base_us
    }
    if (target == vd_frame) return 0;       // still inside this frame
    vd_frame = target;
    if (vd_frame >= vd_frame_count) vd_frame = vd_frame_count - 1;
    vd_decoded_frame = -1;                  // force re-decode on next draw
    return 1;
}
