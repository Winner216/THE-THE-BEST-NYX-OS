// cpu/gamesnd.c — kernel-side mixer for ring-3 game sound effects.
//
// DOOM.EXE (ring-3) uploads its DSP sound lumps (8-bit unsigned mono,
// typically 11 025 Hz) once via SYS_SNDLOAD and triggers voices via
// SYS_SNDPLAY.  Every PIT tick (audio_tick, ~50 Hz) the active voices
// are mixed into one 48 kHz stereo slot and handed to the AC'97 DMA
// ring — the same low-latency interrupt-driven path the video player
// uses.
//
// Memory safety: the submitting task's user pages are only readable
// while it owns the CR3, so SYS_SNDLOAD copies the PCM into the
// kernel heap inside the syscall; the IRQ-context renderer touches
// ONLY kernel-owned memory.  gamesnd_task_exit() stops a task's
// voices and frees its cache wherever its PCB is freed.

#include <gamesnd.h>
#include <ac97.h>
#include <audio.h>
#include <heap.h>
#include <stdint.h>
#include <stddef.h>

#define GS_RATE           48000
#define GS_VOICES         16
#define GS_HANDLES        64
#define GS_PID_CACHE_MAX  (1u << 20)     // 1 MiB cached PCM per task
#define GS_LUMP_MAX       (128 * 1024)   // one sfx sample cap

// DOOM's DSP header is 8 bytes (u16 format=3, u16 rate, u32 len);
// uploads are already stripped, but keep a floor for sanity.
#define GS_PCM_MIN        16

// ─── State ──────────────────────────────────────────────────────
typedef struct {
    int       used;
    int       pid;
    uint8_t*  data;        // kernel copy of the 8-bit unsigned PCM
    int       nsamp;
    int       rate;
} gs_handle_t;

typedef struct {
    int       used;
    int       pid;
    int       chan;
    int       handle;
    uint32_t  pos;         // 16.16 source position
    uint32_t  step;        // (rate << 16) / GS_RATE
    int       amp_l, amp_r;  // 0..128 voice gain (vol × pan)
} gs_voice_t;

static gs_handle_t gs_handles[GS_HANDLES];
static gs_voice_t  gs_voices[GS_VOICES];

static int         gs_plays = 0;

// ─── Load (SYS_SNDLOAD, task context) ───────────────────────────
// Returns a handle ≥ 0, -2 when AC'97 is absent (lets the game's
// sound module Init() fail cleanly so DOOM runs silent), or -1 on
// invalid args / the 1 MiB-per-task cache being full.
// pcm is 8-bit unsigned mono.
int gamesnd_load(int pid, const uint8_t* pcm, int nbytes, int rate) {
    if (!ac97_present()) return -2;
    if (pid <= 0 || !pcm || nbytes < GS_PCM_MIN || nbytes > GS_LUMP_MAX)
        return -1;
    if (rate < 4000 || rate > GS_RATE) return -1;

    int freeh = -1;
    uint32_t pid_bytes = 0;
    for (int i = 0; i < GS_HANDLES; i++) {
        if (gs_handles[i].used) {
            if (gs_handles[i].pid == pid) pid_bytes += gs_handles[i].nsamp;
        } else if (freeh < 0) {
            freeh = i;
        }
    }
    if (freeh < 0 || pid_bytes + (uint32_t)nbytes > GS_PID_CACHE_MAX)
        return -1;

    uint8_t* buf = (uint8_t*)kmalloc((uint32_t)nbytes);
    if (!buf)
        return -1;
    for (int i = 0; i < nbytes; i++) buf[i] = pcm[i];

    gs_handle_t* h = &gs_handles[freeh];
    h->used = 1; h->pid = pid; h->data = buf;
    h->nsamp = nbytes; h->rate = rate;
    return freeh;
}

// ─── Voice control (SYS_SNDPLAY/STOP/PARAMS, task context) ──────
static gs_voice_t* gs_find_voice(int pid, int chan) {
    for (int i = 0; i < GS_VOICES; i++)
        if (gs_voices[i].used && gs_voices[i].pid == pid &&
            gs_voices[i].chan == chan)
            return &gs_voices[i];
    return 0;
}

static void gs_fill_voice(gs_voice_t* v, int pid, int chan, int handle,
                          int vol, int sep) {
    gs_handle_t* h = &gs_handles[handle];
    v->used   = 1;
    v->pid    = pid;
    v->chan   = chan;
    v->handle = handle;
    v->pos    = 0;
    v->step   = (uint32_t)(((uint64_t)h->rate << 16) / GS_RATE);
    // Pan: sep 128 = centre (DOOM NORM_SEP), 0 = full left, 254 = full
    // right.  Centre keeps BOTH channels at full gain — DOOM plays
    // centered sounds on both speakers at volume.
    if (sep < 0)   sep = 0;
    if (sep > 254) sep = 254;
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    int lg = (sep <= 128) ? 128 : 254 - sep;
    int rg = (sep >= 128) ? 128 : sep;
    if (lg < 0) lg = 0;
    if (rg < 0) rg = 0;
    v->amp_l = lg * vol / 127;
    v->amp_r = rg * vol / 127;
}

int gamesnd_play(int pid, int handle, int vol, int sep, int chan) {
    if (handle < 0 || handle >= GS_HANDLES) return -1;
    gs_handle_t* h = &gs_handles[handle];
    if (!h->used || h->pid != pid) return -1;
    if (chan < 0 || chan >= GS_VOICES) return -1;

    gs_voice_t* v = gs_find_voice(pid, chan);
    if (!v) {
        for (int i = 0; i < GS_VOICES; i++)
            if (!gs_voices[i].used) { v = &gs_voices[i]; break; }
        if (!v) {
            // All 16 busy: steal the voice furthest into its sample
            // (least of the remaining audio — the least valuable).
            uint32_t best = 0;
            for (int i = 0; i < GS_VOICES; i++) {
                gs_voice_t* w = &gs_voices[i];
                gs_handle_t* wh = &gs_handles[w->handle];
                uint32_t left = wh->nsamp - (w->pos >> 16);
                if (left >= best) { best = left; v = w; }
            }
            if (!v) return -1;
        }
    }
    gs_fill_voice(v, pid, chan, handle, vol, sep);
    gs_plays++;
    return 0;
}

int gamesnd_stop(int pid, int chan) {
    gs_voice_t* v = gs_find_voice(pid, chan);
    if (!v) return -1;
    v->used = 0;
    return 0;
}

int gamesnd_params(int pid, int chan, int vol, int sep) {
    gs_voice_t* v = gs_find_voice(pid, chan);
    if (!v) return -1;
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    if (sep < 0)   sep = 0;
    if (sep > 254) sep = 254;
    int lg = (sep <= 128) ? 128 : 254 - sep;
    int rg = (sep >= 128) ? 128 : sep;
    if (lg < 0) lg = 0;
    if (rg < 0) rg = 0;
    v->amp_l = lg * vol / 127;
    v->amp_r = rg * vol / 127;
    return 0;
}

int gamesnd_isplaying(int pid, int chan) {
    return gs_find_voice(pid, chan) ? 1 : 0;
}

void gamesnd_task_exit(int pid) {
    if (pid <= 0) return;
    for (int i = 0; i < GS_VOICES; i++)
        if (gs_voices[i].used && gs_voices[i].pid == pid)
            gs_voices[i].used = 0;
    for (int i = 0; i < GS_HANDLES; i++) {
        if (gs_handles[i].used && gs_handles[i].pid == pid) {
            kfree(gs_handles[i].data);
            gs_handles[i].used = 0;
            gs_handles[i].data = 0;
        }
    }
}

// ─── Renderer (audio_tick, IRQ context) ─────────────────────────
int gamesnd_voices_active(void) {
    int n = 0;
    for (int i = 0; i < GS_VOICES; i++)
        if (gs_voices[i].used) n++;
    return n;
}

void gamesnd_render_slot(int16_t* out) {
    audio_gain_service();                 // ease toward sound_volume
    for (int f = 0; f < AC97_SLOT_FRAMES; f++) {
        int32_t l = 0, r = 0;
        for (int vi = 0; vi < GS_VOICES; vi++) {
            gs_voice_t* v = &gs_voices[vi];
            if (!v->used) continue;
            gs_handle_t* h = &gs_handles[v->handle];
            uint32_t idx = v->pos >> 16;
            if (idx >= (uint32_t)h->nsamp) { v->used = 0; continue; }
            uint32_t fr = v->pos & 0xFFFF;
            uint32_t idx1 = idx + 1;
            if (idx1 >= (uint32_t)h->nsamp) idx1 = idx;
            // 8-bit unsigned → signed, linear interp between samples
            int s = ((int)h->data[idx] - 128) * (int)(65536 - fr)
                  + ((int)h->data[idx1] - 128) * (int)fr;
            s >>= 16;
            l += s * v->amp_l;
            r += s * v->amp_r;
            v->pos += v->step;
        }
        // amp ≤ 128, |s| ≤ 128 → |s*amp| ≤ 16384; ×2 ≈ full scale,
        // multiple voices sum then clip in the gain limiter.
        out[f * 2]     = audio_gain_apply(l * 2);
        out[f * 2 + 1] = audio_gain_apply(r * 2);
    }
}
