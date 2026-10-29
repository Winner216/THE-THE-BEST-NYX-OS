// cpu/audio.c — PCM playback for Nyx OS
//
// Two output paths:
//   1. AC'97 (preferred): the raw 8-bit 6 kHz mono sidecar produced by
//      tools/transcode-videos.py is resampled to 16-bit signed stereo
//      @ 48 kHz (8x linear interpolation) and streamed through the
//      codec's DMA ring (cpu/ac97.c).  Real hardware-quality audio —
//      no robot voice.
//   2. PC speaker (fallback): the classic square-wave "robot voice",
//      each sample mapped to a pitch.  Used only when QEMU was started
//      without the AC'97 device.
//
// audio_tick() runs inside the PIT IRQ (~50 Hz) and pushes exactly one
// 20 ms slot of frames per tick; the refill therefore has interrupt
// priority (above every task) and the DMA ring buffers ~80 ms ahead,
// so UI stalls can never cause audio dropouts.

#include <audio.h>
#include <speaker.h>
#include <ac97.h>
#include <mp3async.h>
#include <gamesnd.h>
#include <stdint.h>
#include <stddef.h>

// AC'97 output runs at 48 kHz; the sidecar is AUDIO_PCM_RATE.  The
// upsampling ratio must be integral (6000 -> 8x).  gcc turns the
// division by this constant into shifts at -O2.
#define AC97_UPSCALE (48000 / AUDIO_PCM_RATE)
_Static_assert(48000 % AUDIO_PCM_RATE == 0, "AC97 upsample ratio must be integral");

static const uint8_t* a_data    = NULL;
static int            a_len     = 0;
static int            a_rate    = 0;    // samples per second
static int            a_pos     = 0;    // next sample index
static int            a_playing = 0;
static int            a_muted   = 0;
static int32_t        a_cur     = 0;    // current (slewed) speaker freq, Hz

// ─── Software volume ────────────────────────────────────────────
// The kernel-wide sound_volume knob (0-100) is applied PER-SAMPLE in
// the renderers through a smoothed 16.16 gain, so slider moves glide
// to the new level instead of stepping (the AC'97 codec register
// only has 31 coarse steps).  a_gain eases ~1/8 of the gap per
// render call (~160 ms glide); a_muted forces the target to 0 so
// mute is instant at the DAC too.
static int32_t a_gain = 65536;      // 16.16 smoothed gain, 1.0 = full

static int32_t audio_gain_target(void) {
    extern int sound_volume;        // speaker.h — kernel-wide 0-100 knob
    int v = sound_volume;
    if (v < 0) v = 0; else if (v > 100) v = 100;
    if (a_muted) v = 0;
    return v * 65536 / 100;
}
// Ease the smoothed gain toward its target; exported for the game
// sound mixer (cpu/gamesnd.c) which renders inside the same PIT tick.
void audio_gain_service(void) {
    int32_t tgt = audio_gain_target();
    a_gain += (tgt - a_gain) >> 3;
    if (a_gain < 0) a_gain = 0; else if (a_gain > 65536) a_gain = 65536;
}
// Call once per render call; eases the smoothed gain toward target.
static void audio_gain_tick(void) {
    audio_gain_service();
}
// Apply the current smoothed gain + hard clip to a sample.
int16_t audio_gain_apply(int32_t s) {
    int32_t g = (int32_t)(((int64_t)s * a_gain) >> 16);
    if (g >  32767) g =  32767;
    if (g < -32768) g = -32768;
    return (int16_t)g;
}

// ─── MP3 source mode ────────────────────────────────────────────
// When a_mp3 is set, the audio comes from the background MP3 decoder
// ring (cpu/mp3async.c): 44.1 kHz stereo int16.  audio_tick() pulls
// frames from the ring, resamples 44.1k -> 48k with a 4-tap
// Catmull-Rom cubic SRC, and pushes one AC'97 slot.  The video
// player's .pcm sidecar path is untouched.  a_mono downmixes the
// decoded stereo pair to mono (L+R)/2 for mono sources/PC speakers.
static int a_mp3 = 0;
static int a_mono = 0;              // 1 = mix both channels to mono
// Deferred MP3 DMA start (audio_begin_mp3 / audio_tick): the AC'97
// channel is held until the worker ring holds a full preload of REAL
// decoded frames, so the first ~80 ms the DAC plays is soundtrack,
// not silence — no click at the start of the track (silence → signal
// step) and no perceived delay from a silent lead-in.
#define MP3_START_FRAMES 3600   // ≈ preload source need (3528) + taps
static int a_start_pending = 0; // 1 = waiting for the ring to fill
static int a_start_ticks   = 0; // ticks waited (safety cap — start anyway)
// 16.16 fractional source position, kept < 1.0 between ticks so the
// cubic interpolation never drifts across slot edges.
static uint32_t a_mp3_frac = 0;
// One frame of source history (both channels) for the p0 tap at the
// start of each window — the frame just before the current read.
static int16_t a_mp3_prev[2] = { 0, 0 };
// (44100 << 16) / 48000 rounded = 60211.2 -> 60211
#define MP3_TO_48K_STEP 60211

// ─── AC'97 rendering ─────────────────────────────────────────────
// Render `nframes` 48 kHz stereo frames starting at source sample
// a_pos, 8x upsampled with linear interpolation (fixed point: each
// source sample fans out to 8 frames that glide to the next sample).
// Past the end of the track the tail is padded with silence.  The
// read cursor advances by nframes/8 source samples.
static void audio_render_ac97(int16_t* out, int nframes) {
    audio_gain_tick();
    for (int f = 0; f < nframes; f++) {
        int idx = a_pos + f / AC97_UPSCALE;
        int fr  = f % AC97_UPSCALE;
        int s0, s1;
        if (idx >= a_len) { s0 = 0; s1 = 0; }
        else {
            s0 = (int)a_data[idx] - 128;
            s1 = (idx + 1 < a_len) ? (int)a_data[idx + 1] - 128 : s0;
        }
        int v = (s0 * (AC97_UPSCALE - fr) + s1 * fr) / AC97_UPSCALE;
        int16_t sv = audio_gain_apply(v << 8);    // 8-bit -> 16-bit signed
        out[f * 2]     = sv;                      // left
        out[f * 2 + 1] = sv;                      // right (mono source)
    }
    a_pos += nframes / AC97_UPSCALE;
    if (a_pos > a_len) a_pos = a_len;
}

// Preload for ac97_begin(): the first 4 slots (80 ms) so the DMA
// engine has data the instant the channel starts.
#define AC97_PRELOAD_FRAMES (AC97_SLOT_FRAMES * 4)

// ─── MP3 source: 44.1k -> 48k cubic SRC ─────────────────────────
// A real software sample-rate converter: renders `nframes` 48 kHz
// stereo frames by interpolating the 44.1 kHz ring with a 4-tap
// Catmull-Rom (cubic Hermite) kernel in 16.16 fixed point.  Cubic
// interpolation has a much flatter passband than the old 2-tap
// linear lerp, so 44.1 kHz content keeps its highs at the 48 kHz
// DAC rate without imaging.  The 16.16 phase accumulator is held
// across calls (a_mp3_frac) so consecutive slots join seamlessly,
// and one frame of history (a_mp3_prev) supplies the p0 tap at each
// window start.  The read requests phase-consumed +3 frames: the +3
// covers the two cubic tail taps (i+1, i+2) plus one frame of
// lookahead, so every output frame interpolates between real source
// samples — no clamped seam at slot edges.  The ~2 frame/slot
// lookahead drain is absorbed by the 1.49 s ring (the worker keeps
// it full), and the sub-frame phase keeps the pitch exact.  Missing
// ring data renders as silence (the worker fills it in).
//
// Catmull-Rom (t in [0,1)):
//   y = 0.5 * (2*p1 + (p2-p0)*t + (2p0-5p1+4p2-p3)*t² + (3p1-p0-3p2+p3)*t³)
// With t = fr/65536 the formula stays exact in 16.16 fixed point.
static void audio_render_mp3(int16_t* out, int nframes) {
    audio_gain_tick();
    // Source frames this slot's phase advance consumes, +3 cubic taps.
    uint32_t phase0    = a_mp3_frac;
    uint32_t phase_end = phase0 + (uint32_t)nframes * MP3_TO_48K_STEP;
    int need = (int)(phase_end >> 16) + 3;
    if (need < 4) need = 4;
    if (need > AC97_SLOT_FRAMES + 4) need = AC97_SLOT_FRAMES + 4;
    static int16_t src[AC97_SLOT_FRAMES + 8][2];
    int got = mp3_async_read((int16_t*)src, need);
    if (got <= 0) {                       // ring empty (startup) —
        for (int f = 0; f < nframes; f++) // render silence, keep phase
            out[f*2] = out[f*2+1] = 0;
        a_mp3_frac = phase_end & 0xFFFF;
        a_mp3_prev[0] = a_mp3_prev[1] = 0;
        return;
    }
    // Zero-pad a starved tail so stale static data never leaks in.
    for (int i = got; i < need; i++) src[i][0] = src[i][1] = 0;
    int32_t last = got - 1;
    uint32_t frac = phase0;
    for (int f = 0; f < nframes; f++) {
        uint32_t i  = frac >> 16;
        uint32_t fr = frac & 0xFFFF;
        if (i >= (uint32_t)got) i = (uint32_t)got - 1;   // clamp tail
        int32_t t = (int32_t)fr;
        // t²/t³ in 16.16 (t ≤ 65535 so t*t fits int64)
        int64_t t2 = ((int64_t)t * t) >> 16;
        int64_t t3 = (t2 * t) >> 16;
        int32_t L, R;
        {
            int32_t p0 = (i == 0) ? a_mp3_prev[0] : src[i-1][0];
            int32_t p1 = src[i][0];
            int32_t p2 = (i + 1 < (uint32_t)got) ? src[i+1][0] : src[last][0];
            int32_t p3 = (i + 2 < (uint32_t)got) ? src[i+2][0] : src[last][0];
            L = (int32_t)((((int64_t)p1 << 1) * 65536
                 + (int64_t)(p2 - p0) * t
                 + (int64_t)(2*p0 - 5*p1 + 4*p2 - p3) * t2
                 + (int64_t)(3*p1 - p0 - 3*p2 + p3) * t3) >> 17);
        }
        {
            int32_t p0 = (i == 0) ? a_mp3_prev[1] : src[i-1][1];
            int32_t p1 = src[i][1];
            int32_t p2 = (i + 1 < (uint32_t)got) ? src[i+1][1] : src[last][1];
            int32_t p3 = (i + 2 < (uint32_t)got) ? src[i+2][1] : src[last][1];
            R = (int32_t)((((int64_t)p1 << 1) * 65536
                 + (int64_t)(p2 - p0) * t
                 + (int64_t)(2*p0 - 5*p1 + 4*p2 - p3) * t2
                 + (int64_t)(3*p1 - p0 - 3*p2 + p3) * t3) >> 17);
        }
        if (a_mono) { int32_t m = (L + R) >> 1; L = m; R = m; }
        out[f * 2]     = audio_gain_apply(L);
        out[f * 2 + 1] = audio_gain_apply(R);
        frac += MP3_TO_48K_STEP;
    }
    a_mp3_frac = phase_end & 0xFFFF;   // keep only the sub-frame phase
    a_mp3_prev[0] = src[last][0];      // frame before the NEXT window
    a_mp3_prev[1] = src[last][1];
}

// Begin playback from the MP3 ring (44.1 kHz stereo).  `stream` is the
// assembled elementary stream; the worker task decodes it in the
// background while audio_tick drains the ring.  The AC'97 channel is
// started DEFERRED: audio_tick holds it until the worker ring holds a
// full preload of real decoded frames, then begins with actual
// soundtrack (no silent lead-in, no click).
void audio_begin_mp3(const uint8_t* stream, int len, int pos_ms) {
    if (stream == NULL || len <= 0) { audio_stop_mp3(); return; }
    mp3_async_start(stream, len);
    a_mp3 = 1;
    a_mp3_frac = 0;
    a_mp3_prev[0] = a_mp3_prev[1] = 0;
    a_gain = audio_gain_target();   // jump straight to the current volume
    a_playing = 1;
    a_cur = 0;
    if (pos_ms > 0) mp3_async_seek_ms(pos_ms);   // resume at position
    if (ac97_present()) {
        ac97_mute(a_muted);
        // Deferred start — see the MP3_START_FRAMES comment above.
        a_start_pending = 1;
        a_start_ticks = 0;
    }
}

void audio_begin(const uint8_t* pcm, int len, int rate, int pos_ms) {
    a_data = pcm;
    a_len  = len;
    a_rate = rate;
    a_pos  = 0;
    a_start_pending = 0;            // sidecar path starts immediately
    a_gain = audio_gain_target();   // jump straight to the current volume
    if (rate > 0 && pos_ms > 0)
        a_pos = (int)((int64_t)pos_ms * rate / 1000);
    if (a_pos < 0) a_pos = 0;
    if (a_pos >= a_len) a_pos = 0;
    a_playing = (a_data && a_len > 0 && a_rate > 0);
    a_cur = 0;                     // start each track from a fresh glide

    if (ac97_present()) {
        ac97_mute(a_muted);
        if (!a_playing) {          // nothing to play -> halt the channel
            ac97_stop();
            return;
        }
        // Short critical section: audio_begin runs in the main loop
        // while audio_tick may be mid-render inside the PIT IRQ.
        asm volatile("cli");
        static int16_t pre[AC97_PRELOAD_FRAMES * 2];
        audio_render_ac97(pre, AC97_PRELOAD_FRAMES);
        ac97_begin(pre, AC97_PRELOAD_FRAMES);
        asm volatile("sti");
        if (a_pos >= a_len) a_playing = 0;   // track shorter than preload
        return;
    }
    if (!a_playing) speaker_stop();
}

void audio_stop(void) {
    a_playing = 0;
    a_cur = 0;
    a_start_pending = 0;
    if (ac97_present()) { ac97_stop(); return; }
    speaker_stop();
}

// Stop the MP3 source (video_close) — halts the background worker and
// clears mp3 mode; the AC'97 channel was already stopped by audio_stop.
void audio_stop_mp3(void) {
    if (!a_mp3) return;
    mp3_async_stop();
    a_mp3 = 0;
    a_playing = 0;
    a_mp3_frac = 0;
    a_start_pending = 0;
}

void audio_set_pos_ms(int ms) {
    if (a_mp3) { mp3_async_seek_ms(ms); return; }
    if (a_rate <= 0 || a_len <= 0) return;   // keep a_pos in [0, a_len)
    a_pos = (int)((int64_t)ms * a_rate / 1000);
    if (a_pos < 0) a_pos = 0;
    if (a_pos >= a_len) a_pos = a_len - 1;
}

int audio_has_audio(void) {
    if (a_mp3) return 1;
    return a_data && a_len > 0 && a_rate > 0;
}
int audio_is_playing(void) { return a_playing; }
int audio_elapsed_ms(void) {
    if (a_mp3) return mp3_async_elapsed_ms();
    return (a_rate > 0) ? (int)((int64_t)a_pos * 1000 / a_rate) : 0;
}

// Audio PTS in µs — the master clock's AUDIO reference.  When the
// MP3 ring is active this is the worker's block PTS (mp3_async_pts_us);
// the sidecar path derives it from the sample read position.  Shared
// with the video player so frame PTS and audio PTS live on one µs
// timeline (cpu/video.c vd_master_us()).
int64_t audio_elapsed_us(void) {
    if (a_mp3) return mp3_async_pts_us();
    return (a_rate > 0) ? (int64_t)a_pos * 1000000 / a_rate : 0;
}

void audio_mute(int m) {
    a_muted = m ? 1 : 0;
    if (ac97_present()) { ac97_mute(a_muted); return; }
    if (a_muted) speaker_stop();
}
int audio_is_muted(void) { return a_muted; }

// ─── Volume + channel mixing ─────────────────────────────────────
// The kernel-wide sound_volume knob (0-100) is applied per-sample by
// the renderers through a smoothed gain, so these are just the
// caller-friendly setters used by the video player keys.  The mono
// flag downmixes the decoded stereo pair to (L+R)/2 — handy for the
// PC-speaker fallback or mono soundtracks.
void audio_set_volume(int v) {
    extern int sound_volume;
    if (v < 0) v = 0; else if (v > 100) v = 100;
    sound_volume = v;               // renderers ease a_gain toward it
}
int audio_get_volume(void) {
    extern int sound_volume;
    return sound_volume;
}
void audio_set_mono(int m) { a_mono = m ? 1 : 0; }
int  audio_is_mono(void)   { return a_mono; }

// ─── PC-speaker path ─────────────────────────────────────────────
// Map an 8-bit unsigned sample to a target speaker pitch.  128 =
// silence; deviation from centre drives the pitch upward.  Square-law
// spread keeps quiet content audible and loud content bright.
static uint32_t audio_target(int s) {
    int dev = s - 128;
    if (dev < 0) dev = -dev;
    uint32_t f = 140u + (uint32_t)(dev * dev) / 12u;
    if (f > 1300) f = 1300;
    return f;
}

extern void wasapi_service(void);  // wasapi.c — waveOut ring drain

// ─── Game-sound path (DOOM) ─────────────────────────────────────
// While the game has voices (or is draining its tail), game sound
// owns the AC'97 channel: mix one slot per tick and push.  After the
// last voice finishes we keep pushing silence for AC97_NSLOTS ticks
// so the DMA cursor plays out the ring before we release the channel
// back to the video/MP3 path (no click at the handoff).
static int gs_dma_on = 0;
static int gs_hold  = 0;

void audio_tick(void) {
    // Always drain waveOut PCM rings into the AC97 DMA engine,
    // even when the video player isn't active (waveOut is independent).
    if (ac97_present()) wasapi_service();

    // ── Game sound: highest priority while voices are live ──────
    if (gamesnd_voices_active() > 0 || gs_hold > 0) {
        if (ac97_present()) {
            if (!gs_dma_on) {
                // Deferred start: preload 4 slots of REAL mixed audio
                // so the DAC starts mid-signal, not on a silence step.
                static int16_t gpre[AC97_PRELOAD_FRAMES * 2];
                int16_t* p = gpre;
                for (int s = 0; s < 4; s++) {
                    gamesnd_render_slot(p);
                    p += AC97_SLOT_FRAMES * 2;
                }
                ac97_begin(gpre, AC97_PRELOAD_FRAMES);
                gs_dma_on = 1;
            } else {
                static int16_t gsc[AC97_SLOT_FRAMES * 2];
                gamesnd_render_slot(gsc);
                ac97_push(gsc, AC97_SLOT_FRAMES);
                ac97_service(1);
            }
        }
        if (gamesnd_voices_active() > 0) gs_hold = AC97_NSLOTS;
        else                             gs_hold--;
        return;
    }
    if (gs_dma_on) {                     // tail drained — release the
        ac97_stop();                     // channel for other sources
        gs_dma_on = 0;
    }

    if (!a_playing) return;

    // ── MP3 source: pull from the worker ring, resample, push ────
    if (a_mp3) {
        if (ac97_present()) {
            // Deferred start: hold the channel until the ring holds a
            // full preload of real audio (or the stream ended / a
            // safety cap elapsed — start with what's available,
            // silence-padded), then begin with actual frames.  The
            // preload render consumes the frames it needs from the
            // ring, so the DMA starts mid-track, not from silence.
            if (a_start_pending) {
                if (mp3_async_available() >= MP3_START_FRAMES ||
                    mp3_async_eos() || ++a_start_ticks > 30) {
                    a_start_pending = 0;
                    static int16_t pre[AC97_PRELOAD_FRAMES * 2];
                    audio_render_mp3(pre, AC97_PRELOAD_FRAMES);
                    ac97_begin(pre, AC97_PRELOAD_FRAMES);
                }
                return;
            }
            static int16_t scratch[AC97_SLOT_FRAMES * 2];
            audio_render_mp3(scratch, AC97_SLOT_FRAMES);
            ac97_push(scratch, AC97_SLOT_FRAMES);
            ac97_service(1);
            // End of stream: keep playing what's already in the AC'97
            // DMA ring, then stop pulling when the source is drained.
            if (mp3_async_eos() && mp3_async_available() == 0) {
                a_playing = 0;
                a_cur = 0;
            }
        }
        return;
    }

    if (!a_data || a_len <= 0 || a_rate <= 0) return;

    int per_tick = a_rate / 50;               // ~50 Hz system tick
    if (per_tick < 1) per_tick = 1;
    if (per_tick > 512) per_tick = 512;       // IRQ-loop insurance

    // ── AC'97 path: render one 20 ms slot and hand it to the DMA ──
    if (ac97_present()) {
        static int16_t scratch[AC97_SLOT_FRAMES * 2];
        int nframes = per_tick * AC97_UPSCALE;
        if (nframes > AC97_SLOT_FRAMES) nframes = AC97_SLOT_FRAMES;
        if (nframes <= 0) return;
        audio_render_ac97(scratch, nframes);
        ac97_push(scratch, nframes);
        ac97_service(1);
        if (a_pos >= a_len) {                 // track finished — the
            a_playing = 0;                    // ring plays out the tail
            a_cur = 0;
        }
        return;
    }

    // ── PC-speaker fallback path ─────────────────────────────────
    for (int i = 0; i < per_tick; i++) {
        if (a_pos >= a_len) {                 // end of track
            a_playing = 0;
            a_cur = 0;
            speaker_stop();
            return;
        }
        int s = a_data[a_pos++];
        if (a_muted) continue;
        int dev = s - 128;
        if (dev < 0) dev = -dev;
        if (dev < 8) {                        // near-silence gap
            a_cur = 0;
            speaker_stop();
            continue;
        }
        uint32_t target = audio_target(s);
        if (a_cur == 0) a_cur = (int32_t)target;
        else a_cur += ((int32_t)target - a_cur) >> 3;   // ~1.3 ms glide
        speaker_play((uint32_t)a_cur);
    }
}
