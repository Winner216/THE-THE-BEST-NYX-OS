// cpu/wasapi.c — Windows Core Audio / WASAPI compatibility for Nyx OS
//
// Three tiers of audio API, all backed by the kernel's AC97 driver:
//   1. waveOut/waveIn  — classic MME (wide compat)
//   2. WASAPI          — modern session API (low latency)
//   3. Mixer API       — volume/mute control
//
// Audio buffers are copied into a kernel-side ring and drained by the
// PIT IRQ at 48 kHz through the AC97 DMA engine.  The waveOut Write
// call is non-blocking: it copies the caller's buffer and returns
// immediately, while the audio_tick() path in audio.c pushes frames
// into the BDL ring at interrupt priority.

#include <wasapi.h>
#include <audio.h>
#include <ac97.h>
#include <timer.h>
#include <heap.h>
#include <string.h>
#include <io.h>
#include <stdint.h>
#include <stddef.h>

// ═══════════════════════════════════════════════════════════════
// waveOut / waveIn — Classic MME Audio
// ═══════════════════════════════════════════════════════════════

// ─── Device table ───────────────────────────────────────────────
#define WAVE_MAX_HANDLES 8

typedef struct {
    int              in_use;
    WAVEFORMATEX     format;
    DWORD            callback;       // callback function pointer
    DWORD_PTR        instance;       // user data for callback
    int              paused;
    int              prepared;       // header list root
    int              prepared_count;
    int              buffer_write_idx; // circular write index
    int              buffer_read_idx;  // circular read index
    int              buffer_count;   // number of queued buffers
#define WAVE_BUF_QUEUE 16
    LPWAVEHDR         buf_queue[WAVE_BUF_QUEUE];
    // Internal PCM ring for the AC97 render path
    int16_t*          pcm_ring;
    uint32_t          pcm_ring_len;  // total int16 samples in ring
    uint32_t          pcm_write_pos;
    uint32_t          pcm_read_pos;
    uint32_t          pcm_available;
    // Volume (0x0000-0xFFFF per channel, low word = left, high = right)
    DWORD             volume;
    DWORD             saved_volume;
} wave_handle_t;

static wave_handle_t wave_out_handles[WAVE_MAX_HANDLES];
static wave_handle_t wave_in_handles[WAVE_MAX_HANDLES];

// PCM ring size: enough for ~200 ms at 48 kHz stereo = ~19200 frames = 38400 int16
#define WAVE_PCM_RING_FRAMES 19200
#define WAVE_PCM_RING_SAMPLES (WAVE_PCM_RING_FRAMES * 2)

// ─── waveOut helpers ────────────────────────────────────────────
static wave_handle_t* wave_alloc_out(void) {
    for (int i = 0; i < WAVE_MAX_HANDLES; i++) {
        if (!wave_out_handles[i].in_use) {
            wave_handle_t* h = &wave_out_handles[i];
            memset(h, 0, sizeof(*h));
            h->in_use = 1;
            h->volume = 0xFFFFFFFF;  // full volume both channels
            return h;
        }
    }
    return NULL;
}

static wave_handle_t* wave_find_out(HWAVEOUT hwo) {
    int idx = (int)(uintptr_t)hwo;
    if (idx < 0 || idx >= WAVE_MAX_HANDLES) return NULL;
    wave_handle_t* h = &wave_out_handles[idx];
    return h->in_use ? h : NULL;
}

static void wave_free_out(wave_handle_t* h) {
    if (h->pcm_ring) { kfree(h->pcm_ring); h->pcm_ring = NULL; }
    h->in_use = 0;
}

// Convert MME buffer data to 16-bit stereo PCM and push to ring.
// Handles 8-bit and 16-bit source, mono and stereo, and any sample rate.
static void wave_push_buffer(wave_handle_t* h, LPWAVEHDR hdr) {
    if (!h->pcm_ring) return;
    BYTE* src = (BYTE*)hdr->lpData;
    DWORD len = hdr->dwBufferLength;
    if (!src || len == 0) return;

    int channels  = h->format.nChannels;
    int bps       = h->format.wBitsPerSample;
    int bytes_per_sample = bps / 8;
    int bytes_per_frame  = channels * bytes_per_sample;

    if (bytes_per_frame == 0) return;
    DWORD nframes = len / bytes_per_frame;

    // Sample rate conversion factor: source_rate / 48000 in 16.16 fixed point
    // AC97 runs at 48 kHz; source may be any rate.
    uint32_t src_rate = h->format.nSamplesPerSec;
    uint32_t src_step = (uint32_t)(((uint64_t)src_rate << 16) / 48000);
    if (src_step == 0) src_step = 1;  // safety

    uint32_t frac = 0;
    uint32_t ring_mask = h->pcm_ring_len - 1;

    for (uint32_t out_f = 0; out_f < WAVE_PCM_RING_FRAMES && h->pcm_available < h->pcm_ring_len - 2;) {
        uint32_t src_f = frac >> 16;
        if (src_f >= nframes) break;
        BYTE* frame = src + src_f * bytes_per_frame;

        int16_t L, R;
        if (bps == 8) {
            // 8-bit unsigned -> signed 16-bit
            L = ((int16_t)frame[0] - 128) << 8;
            R = (channels >= 2) ? ((int16_t)frame[1] - 128) << 8 : L;
        } else if (bps == 16) {
            // 16-bit little-endian signed
            L = (int16_t)(frame[0] | (frame[1] << 8));
            R = (channels >= 2) ? (int16_t)(frame[2] | (frame[3] << 8)) : L;
        } else if (bps == 24) {
            int32_t lv = (int32_t)(frame[0] | (frame[1] << 8) | (frame[2] << 16));
            if (lv & 0x800000) lv |= 0xFF000000;  // sign extend
            L = (int16_t)(lv >> 8);
            int32_t rv = channels >= 2 ? (int32_t)(frame[3] | (frame[4] << 8) | (frame[5] << 16)) : lv;
            if (rv & 0x800000) rv |= 0xFF000000;
            R = (int16_t)(rv >> 8);
        } else { // 32-bit
            int32_t lv = (int32_t)(frame[0] | (frame[1] << 8) | (frame[2] << 16) | (frame[3] << 24));
            L = (int16_t)(lv >> 16);
            int32_t rv = channels >= 2 ? (int32_t)(frame[4] | (frame[5] << 8) | (frame[6] << 16) | (frame[7] << 24)) : lv;
            R = (int16_t)(rv >> 16);
        }

        // Apply per-handle volume (0x0000-0xFFFF scale)
        uint32_t vol = h->volume;
        int vol_l = vol & 0xFFFF;
        int vol_h = (vol >> 16) & 0xFFFF;
        L = (int16_t)(((int32_t)L * vol_l) >> 16);
        R = (int16_t)(((int32_t)R * vol_h) >> 16);

        uint32_t wp = h->pcm_write_pos;
        h->pcm_ring[wp] = L;
        h->pcm_ring[wp + 1] = R;
        h->pcm_write_pos = (wp + 2) & ring_mask;
        h->pcm_available++;
        frac += src_step;
    }
}

// Drain the PCM ring and push into the AC97 DMA engine.
// Called from audio_tick() at ~50 Hz via the waveOut service path.
// Returns the number of frames pushed.
static int wave_out_started = 0;  // track AC97 channel state for waveOut

static int wave_out_service_ring(wave_handle_t* h) {
    if (!h || !h->pcm_ring || h->pcm_available == 0 || h->paused)
        return 0;

    // Pull up to AC97_SLOT_FRAMES from the ring
    int16_t frames[AC97_SLOT_FRAMES * 2];
    int nframes = 0;
    uint32_t ring_mask = h->pcm_ring_len - 1;

    while (nframes < AC97_SLOT_FRAMES && h->pcm_available > 0) {
        uint32_t rp = h->pcm_read_pos;
        frames[nframes * 2]     = h->pcm_ring[rp];
        frames[nframes * 2 + 1] = h->pcm_ring[rp + 1];
        h->pcm_read_pos = (rp + 2) & ring_mask;
        h->pcm_available--;
        nframes++;
    }

    if (nframes > 0 && ac97_present()) {
        // First push: start the AC97 DMA channel with a preload.
        // Subsequent pushes just extend the ring.
        if (!wave_out_started) {
            ac97_begin(frames, nframes);
            wave_out_started = 1;
        } else {
            ac97_push(frames, nframes);
        }
        ac97_service(1);
    }
    return nframes;
}

// ─── waveOut exported functions ─────────────────────────────────

UINT waveOutGetNumDevs(void) {
    return ac97_present() ? 1 : 0;
}

MMRESULT waveOutGetDevCapsA(UINT_PTR uDeviceID, LPWAVEOUTCAPS caps, UINT cbcaps) {
    (void)uDeviceID;
    if (!caps || cbcaps < sizeof(WAVEOUTCAPS)) return MMSYSERR_INVALPARAM;
    memset(caps, 0, sizeof(WAVEOUTCAPS));
    caps->wMid = 0x0001;           // "Microsoft" fake ID
    caps->wPid = 0x0001;
    caps->vDriverVersion = 0x0100; // 1.0
    strncpy(caps->szPname, "Nyx AC97", 32);
    caps->dwFormats = WAVE_FORMAT_4S16 | WAVE_FORMAT_4M16 |
                      WAVE_FORMAT_1S16 | WAVE_FORMAT_1M16 |
                      WAVE_FORMAT_2S16 | WAVE_FORMAT_2M16 |
                      WAVE_FORMAT_4S08 | WAVE_FORMAT_4M08;
    caps->wChannels = 2;
    caps->dwSupport = 0;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutOpen(HWAVEOUT* phwo, UINT_PTR uDeviceID,
                     LPCWAVEFORMATEX pwfx, DWORD_PTR dwCallback,
                     DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)uDeviceID;
    (void)fdwOpen;
    if (!phwo || !pwfx) return MMSYSERR_INVALPARAM;
    if (!ac97_present()) return MMSYSERR_NODRIVER;
    // Only PCM format supported
    if (pwfx->wFormatTag != WAVE_FORMAT_PCM) return WAVERR_BADFORMAT;
    if (pwfx->nChannels < 1 || pwfx->nChannels > 2) return WAVERR_BADFORMAT;
    if (pwfx->wBitsPerSample != 8 && pwfx->wBitsPerSample != 16 &&
        pwfx->wBitsPerSample != 24 && pwfx->wBitsPerSample != 32)
        return WAVERR_BADFORMAT;

    wave_handle_t* h = wave_alloc_out();
    if (!h) return MMSYSERR_NOMEM;

    h->format = *pwfx;
    h->callback = (DWORD)(dwCallback & 0xFFFFFFFF);
    h->instance = dwInstance;
    h->paused = 0;

    // Allocate PCM ring
    h->pcm_ring_len = WAVE_PCM_RING_SAMPLES;
    h->pcm_ring = (int16_t*)kmalloc(WAVE_PCM_RING_SAMPLES * sizeof(int16_t));
    if (!h->pcm_ring) { h->in_use = 0; return MMSYSERR_NOMEM; }
    memset(h->pcm_ring, 0, WAVE_PCM_RING_SAMPLES * sizeof(int16_t));
    h->pcm_write_pos = 0;
    h->pcm_read_pos = 0;
    h->pcm_available = 0;

    *phwo = (HWAVEOUT)(uintptr_t)(h - wave_out_handles);

    // Serial debug
    { const char* m = "WAVEOUT OPEN: ok\n";
      for (; *m; m++) { int sp=0; while((inb(0x3F8+5)&0x20)==0&&sp++<200000){} outb(0x3F8,(uint8_t)*m); } }

    return MMSYSERR_NOERROR;
}

MMRESULT waveOutClose(HWAVEOUT hwo) {
    wave_handle_t* h = wave_find_out(hwo);
    if (!h) return MMSYSERR_INVALHANDLE;

    // Stop playback
    h->paused = 0;

    // Mark all queued buffers as done
    for (int i = 0; i < h->buffer_count; i++) {
        LPWAVEHDR hdr = h->buf_queue[i];
        if (hdr) {
            hdr->dwFlags |= WHDR_DONE;
            hdr->dwFlags &= ~WHDR_INQUEUE;
        }
    }
    h->buffer_count = 0;
    h->buffer_read_idx = 0;
    h->buffer_write_idx = 0;

    wave_free_out(h);
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutPrepareHeader(HWAVEOUT hwo, LPWAVEHDR pwh, UINT cbwh) {
    (void)cbwh;
    wave_handle_t* h = wave_find_out(hwo);
    if (!h || !pwh) return MMSYSERR_INVALHANDLE;
    pwh->dwFlags |= WHDR_PREPARED;
    pwh->dwFlags &= ~WHDR_DONE;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutUnprepareHeader(HWAVEOUT hwo, LPWAVEHDR pwh, UINT cbwh) {
    (void)cbwh;
    wave_handle_t* h = wave_find_out(hwo);
    if (!h || !pwh) return MMSYSERR_INVALHANDLE;
    pwh->dwFlags &= ~(WHDR_PREPARED | WHDR_DONE | WHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutWrite(HWAVEOUT hwo, LPWAVEHDR pwh, UINT cbwh) {
    (void)cbwh;
    wave_handle_t* h = wave_find_out(hwo);
    if (!h || !pwh) return MMSYSERR_INVALHANDLE;
    if (!(pwh->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;

    pwh->dwFlags |= WHDR_INQUEUE;
    pwh->dwFlags &= ~WHDR_DONE;

    // Push buffer data into the PCM ring
    wave_push_buffer(h, pwh);

    // Mark as done (synchronous copy model)
    pwh->dwFlags |= WHDR_DONE;
    pwh->dwFlags &= ~WHDR_INQUEUE;

    return MMSYSERR_NOERROR;
}

MMRESULT waveOutPause(HWAVEOUT hwo) {
    wave_handle_t* h = wave_find_out(hwo);
    if (!h) return MMSYSERR_INVALHANDLE;
    h->paused = 1;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutRestart(HWAVEOUT hwo) {
    wave_handle_t* h = wave_find_out(hwo);
    if (!h) return MMSYSERR_INVALHANDLE;
    h->paused = 0;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutReset(HWAVEOUT hwo) {
    wave_handle_t* h = wave_find_out(hwo);
    if (!h) return MMSYSERR_INVALHANDLE;
    h->paused = 0;
    h->pcm_write_pos = 0;
    h->pcm_read_pos = 0;
    h->pcm_available = 0;
    wave_out_started = 0;
    memset(h->pcm_ring, 0, h->pcm_ring_len * sizeof(int16_t));
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutGetPosition(HWAVEOUT hwo, void* pmmt, UINT cbmmt) {
    (void)hwo; (void)pmmt; (void)cbmmt;
    // Return current playback position in bytes (simplified)
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutSetVolume(HWAVEOUT hwo, DWORD dwVolume) {
    wave_handle_t* h = wave_find_out(hwo);
    if (!h) return MMSYSERR_INVALHANDLE;
    h->volume = dwVolume;
    // Also apply to the global sound_volume knob
    int avg = (int)((dwVolume & 0xFFFF) + ((dwVolume >> 16) & 0xFFFF)) / 2;
    audio_set_volume(avg * 100 / 0xFFFF);
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutGetVolume(HWAVEOUT hwo, LPDWORD pdwVolume) {
    wave_handle_t* h = wave_find_out(hwo);
    if (!h || !pdwVolume) return MMSYSERR_INVALHANDLE;
    *pdwVolume = h->volume;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutGetID(HWAVEOUT hwo, UINT* puDeviceID) {
    (void)hwo;
    if (puDeviceID) *puDeviceID = 0;
    return MMSYSERR_NOERROR;
}

UINT waveOutMessage(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dw1, DWORD_PTR dw2) {
    (void)hwo; (void)uMsg; (void)dw1; (void)dw2;
    return MMSYSERR_NOERROR;
}

// ─── waveIn (stub — no capture hardware in QEMU) ───────────────

UINT waveInGetNumDevs(void) {
    return 0;  // QEMU doesn't emulate microphone
}

MMRESULT waveInGetDevCapsA(UINT_PTR uDeviceID, LPWAVEINCAPS caps, UINT cbcaps) {
    (void)uDeviceID;
    if (!caps || cbcaps < sizeof(WAVEINCAPS)) return MMSYSERR_INVALPARAM;
    memset(caps, 0, sizeof(WAVEINCAPS));
    strncpy(caps->szPname, "Nyx AC97 In", 32);
    caps->wChannels = 2;
    return MMSYSERR_NOERROR;
}

MMRESULT waveInOpen(HWAVEIN* phwi, UINT_PTR uDeviceID,
                    LPCWAVEFORMATEX pwfx, DWORD_PTR dwCallback,
                    DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)uDeviceID; (void)pwfx; (void)dwCallback;
    (void)dwInstance; (void)fdwOpen;
    if (phwi) *phwi = NULL;
    return MMSYSERR_NODRIVER;  // no capture hardware
}

MMRESULT waveInClose(HWAVEIN hwi) { (void)hwi; return MMSYSERR_NOERROR; }
MMRESULT waveInPrepareHeader(HWAVEIN hwi, LPWAVEHDR pwh, UINT cbwh) {
    (void)hwi; (void)pwh; (void)cbwh; return MMSYSERR_NOERROR;
}
MMRESULT waveInUnprepareHeader(HWAVEIN hwi, LPWAVEHDR pwh, UINT cbwh) {
    (void)hwi; (void)pwh; (void)cbwh; return MMSYSERR_NOERROR;
}
MMRESULT waveInAddBuffer(HWAVEIN hwi, LPWAVEHDR pwh, UINT cbwh) {
    (void)hwi; (void)pwh; (void)cbwh; return MMSYSERR_NOERROR;
}
MMRESULT waveInStart(HWAVEIN hwi) { (void)hwi; return MMSYSERR_NOERROR; }
MMRESULT waveInStop(HWAVEIN hwi) { (void)hwi; return MMSYSERR_NOERROR; }
MMRESULT waveInReset(HWAVEIN hwi) { (void)hwi; return MMSYSERR_NOERROR; }
MMRESULT waveInGetID(HWAVEIN hwi, UINT* puDeviceID) {
    (void)hwi; if (puDeviceID) *puDeviceID = 0; return MMSYSERR_NOERROR;
}

// ═══════════════════════════════════════════════════════════════
// WASAPI — Modern Session API
// ═══════════════════════════════════════════════════════════════
// Simplified handle-based API.  Each IAudioClient maps to one
// wave_out handle's PCM ring.  IAudioRenderClient is the write
// interface; IAudioCaptureClient is a no-op stub.

#define WASAPI_MAX_CLIENTS 4

typedef struct {
    int              in_use;
    UINT             share_mode;
    DWORD            stream_flags;
    WAVEFORMATEX     format;
    UINT             buffer_frames;  // buffer size in frames
    UINT             period_frames;  // default period in frames
    int              started;
    // Maps to wave_out PCM ring
    wave_handle_t*   wave_handle;
    BYTE*            render_buf;     // shared buffer for GetBuffer/ReleaseBuffer
    UINT             render_buf_size;
} wasapi_client_t;

static wasapi_client_t wasapi_clients[WASAPI_MAX_CLIENTS];

static wasapi_client_t* wasapi_alloc(void) {
    for (int i = 0; i < WASAPI_MAX_CLIENTS; i++) {
        if (!wasapi_clients[i].in_use) {
            memset(&wasapi_clients[i], 0, sizeof(wasapi_client_t));
            wasapi_clients[i].in_use = 1;
            return &wasapi_clients[i];
        }
    }
    return NULL;
}

MMRESULT audc_client_initialize(IAudioClient* client, UINT shareMode,
                                DWORD streamFlags, UINT64 hnsBufferDuration,
                                UINT64 hnsPeriodicity,
                                LPCWAVEFORMATEX pFormat,
                                LPCWAVEFORMATEX* ppMixFormat) {
    (void)hnsBufferDuration; (void)hnsPeriodicity;
    wasapi_client_t* c = (wasapi_client_t*)client;
    if (!c || !pFormat) return MMSYSERR_INVALPARAM;
    if (pFormat->wFormatTag != WAVE_FORMAT_PCM) return AUDCLNT_ERR_UNSUPPORTED_FORMAT;

    c->share_mode = shareMode;
    c->stream_flags = streamFlags;
    c->format = *pFormat;
    if (c->format.cbSize == 0 && sizeof(WAVEFORMATEX) > 0) {
        c->format.cbSize = 0;
    }

    // Default buffer: ~100 ms at the given sample rate
    c->buffer_frames = pFormat->nSamplesPerSec / 10;
    if (c->buffer_frames < 480) c->buffer_frames = 480;  // min 10 ms at 48k
    c->period_frames = pFormat->nSamplesPerSec / 100;   // 10 ms default period

    // Allocate shared render buffer
    c->render_buf_size = c->buffer_frames * pFormat->nChannels * (pFormat->wBitsPerSample / 8);
    c->render_buf = (BYTE*)kmalloc(c->render_buf_size);
    if (c->render_buf) memset(c->render_buf, 0, c->render_buf_size);

    // If mix format requested, provide it
    if (ppMixFormat) {
        static WAVEFORMATEX mix_fmt;
        mix_fmt.wFormatTag = WAVE_FORMAT_PCM;
        mix_fmt.nChannels = 2;
        mix_fmt.nSamplesPerSec = 48000;
        mix_fmt.wBitsPerSample = 16;
        mix_fmt.nBlockAlign = 4;
        mix_fmt.nAvgBytesPerSec = 192000;
        mix_fmt.cbSize = 0;
        *ppMixFormat = &mix_fmt;
    }

    return MMSYSERR_NOERROR;
}

MMRESULT audc_client_get_buffer_size(IAudioClient* client, UINT* pNumBufferFrames) {
    wasapi_client_t* c = (wasapi_client_t*)client;
    if (!c || !pNumBufferFrames) return MMSYSERR_INVALPARAM;
    *pNumBufferFrames = c->buffer_frames;
    return MMSYSERR_NOERROR;
}

MMRESULT audc_client_get_period(IAudioClient* client, UINT* pDefaultDevicePeriod,
                                UINT* pMinimumDevicePeriod) {
    wasapi_client_t* c = (wasapi_client_t*)client;
    if (!c) return MMSYSERR_INVALPARAM;
    if (pDefaultDevicePeriod) *pDefaultDevicePeriod = c->period_frames;
    if (pMinimumDevicePeriod) *pMinimumDevicePeriod = 480;  // 10 ms min
    return MMSYSERR_NOERROR;
}

MMRESULT audc_client_start(IAudioClient* client) {
    wasapi_client_t* c = (wasapi_client_t*)client;
    if (!c) return MMSYSERR_INVALHANDLE;
    c->started = 1;
    return MMSYSERR_NOERROR;
}

MMRESULT audc_client_stop(IAudioClient* client) {
    wasapi_client_t* c = (wasapi_client_t*)client;
    if (!c) return MMSYSERR_INVALHANDLE;
    c->started = 0;
    return MMSYSERR_NOERROR;
}

MMRESULT audc_client_reset(IAudioClient* client) {
    wasapi_client_t* c = (wasapi_client_t*)client;
    if (!c) return MMSYSERR_INVALHANDLE;
    c->started = 0;
    if (c->render_buf) memset(c->render_buf, 0, c->render_buf_size);
    return MMSYSERR_NOERROR;
}

MMRESULT audc_client_get_state(IAudioClient* client, DWORD* pState) {
    wasapi_client_t* c = (wasapi_client_t*)client;
    if (!c || !pState) return MMSYSERR_INVALPARAM;
    *pState = c->started ? 1 : 0;  // 1 = running, 0 = stopped
    return MMSYSERR_NOERROR;
}

MMRESULT audc_client_get_current_padding(IAudioClient* client, UINT* pNumPaddingFrames) {
    wasapi_client_t* c = (wasapi_client_t*)client;
    if (!c || !pNumPaddingFrames) return MMSYSERR_INVALPARAM;
    // Approximate: how much data is in the ring
    *pNumPaddingFrames = 0;
    if (c->wave_handle && c->wave_handle->in_use) {
        *pNumPaddingFrames = c->wave_handle->pcm_available;
    }
    return MMSYSERR_NOERROR;
}

// IAudioRenderClient
MMRESULT audrc_get_buffer(IAudioRenderClient* rc, UINT NumFramesRequested,
                          BYTE** ppData) {
    wasapi_client_t* c = (wasapi_client_t*)rc;
    if (!c || !ppData) return MMSYSERR_INVALPARAM;
    UINT bytes = NumFramesRequested * c->format.nChannels * (c->format.wBitsPerSample / 8);
    if (bytes > c->render_buf_size) bytes = c->render_buf_size;
    if (c->render_buf) {
        memset(c->render_buf, 0, bytes);
        *ppData = c->render_buf;
    } else {
        return MMSYSERR_NOMEM;
    }
    return MMSYSERR_NOERROR;
}

MMRESULT audrc_release_buffer(IAudioRenderClient* rc, UINT NumFramesWritten,
                              DWORD dwFlags) {
    (void)dwFlags;
    wasapi_client_t* c = (wasapi_client_t*)rc;
    if (!c || !c->render_buf) return MMSYSERR_INVALPARAM;

    // Create a fake WAVEHDR and push through the waveOut path
    DWORD bytes_per_frame = c->format.nChannels * (c->format.wBitsPerSample / 8);
    DWORD len = NumFramesWritten * bytes_per_frame;
    if (len > c->render_buf_size) len = c->render_buf_size;

    if (c->wave_handle && c->wave_handle->in_use) {
        WAVEHDR fake_hdr;
        memset(&fake_hdr, 0, sizeof(fake_hdr));
        fake_hdr.lpData = (LPSTR)c->render_buf;
        fake_hdr.dwBufferLength = len;
        fake_hdr.dwFlags = WHDR_PREPARED;
        wave_push_buffer(c->wave_handle, &fake_hdr);
    }
    return MMSYSERR_NOERROR;
}

// IAudioCaptureClient (stubs — no capture in QEMU)
MMRESULT audcc_get_buffer(IAudioCaptureClient* cc, BYTE** ppData,
                          UINT* pNumFramesToRead, DWORD* pdwFlags,
                          UINT64* pqwDevicePosition, UINT64* pqwAudioTimestamp) {
    (void)cc; (void)ppData; (void)pNumFramesToRead;
    (void)pdwFlags; (void)pqwDevicePosition; (void)pqwAudioTimestamp;
    return AUDCLNT_S_BUFFER_EMPTY;  // no data
}

MMRESULT audcc_release_buffer(IAudioCaptureClient* cc, UINT NumFramesRead) {
    (void)cc; (void)NumFramesRead;
    return MMSYSERR_NOERROR;
}

// Factory: activate an interface (simplified)
MMRESULT audc_factory_activate(LPCSTR iid, void** ppInterface) {
    (void)iid;
    if (!ppInterface) return MMSYSERR_INVALPARAM;

    wasapi_client_t* c = wasapi_alloc();
    if (!c) return MMSYSERR_NOMEM;

    // Create a waveOut handle for this client
    WAVEFORMATEX fmt = {0};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = 192000;

    HWAVEOUT hwo;
    MMRESULT mr = waveOutOpen(&hwo, 0, &fmt, 0, 0, 0);
    if (mr != MMSYSERR_NOERROR) {
        c->in_use = 0;
        return mr;
    }
    c->wave_handle = wave_find_out(hwo);

    *ppInterface = (void*)c;
    return MMSYSERR_NOERROR;
}

MMRESULT audc_release(IUnknown* iface) {
    if (!iface) return MMSYSERR_INVALHANDLE;
    // For wasapi_client_t, clean up
    wasapi_client_t* c = (wasapi_client_t*)iface;
    if (c->render_buf) { kfree(c->render_buf); c->render_buf = NULL; }
    // Close the associated waveOut handle
    for (int i = 0; i < WAVE_MAX_HANDLES; i++) {
        if (&wave_out_handles[i] == c->wave_handle) {
            wave_free_out(c->wave_handle);
            break;
        }
    }
    c->in_use = 0;
    return MMSYSERR_NOERROR;
}

// ═══════════════════════════════════════════════════════════════
// Mixer API — Volume/Mute Control
// ═══════════════════════════════════════════════════════════════

// Simplified: one mixer device, one destination line (speakers),
// one source (wave out), one volume control, one mute control.

MMRESULT mixerOpen(HMIXER* phmx, UINT uMxId, DWORD_PTR dwCallback,
                   DWORD_PTR dwInstance, DWORD fdwOpen) {
    (void)uMxId; (void)dwCallback; (void)dwInstance; (void)fdwOpen;
    if (!phmx) return MMSYSERR_INVALPARAM;
    // Return a fake handle
    static int mixer_handle = 0x4D58;  // "MX"
    *phmx = (HMIXER)(uintptr_t)mixer_handle;
    return MMSYSERR_NOERROR;
}

MMRESULT mixerClose(HMIXER hmx) {
    (void)hmx;
    return MMSYSERR_NOERROR;
}

UINT mixerGetNumDevs(void) {
    return 1;  // one virtual mixer
}

MMRESULT mixerGetLineInfoA(HMIXER hmxobj, LPMIXERLINE pmxl, DWORD fdwInfo) {
    (void)hmxobj;
    if (!pmxl) return MMSYSERR_INVALPARAM;

    static const char* dest_name = "Speaker";
    static const char* src_name  = "Wave Out";

    if (fdwInfo == 0) {
        // Query destination line
        pmxl->cbStruct = sizeof(MIXERLINE);
        pmxl->cChannels = 2;
        pmxl->cConnections = 1;
        pmxl->cControls = 2;  // volume + mute
        pmxl->fdwLine = MIXERLINE_LINEF_ACTIVE;
        strncpy(pmxl->szShortName, "Spk", 16);
        strncpy(pmxl->szName, dest_name, 64);
        pmxl->dwType = 0;  // audio
        pmxl->dwLineID = 0;
    } else {
        // Query source line
        pmxl->cbStruct = sizeof(MIXERLINE);
        pmxl->cChannels = 2;
        pmxl->cConnections = 0;
        pmxl->cControls = 2;
        pmxl->fdwLine = MIXERLINE_LINEF_ACTIVE | MIXERLINE_LINEF_SOURCE;
        strncpy(pmxl->szShortName, "Wave", 16);
        strncpy(pmxl->szName, src_name, 64);
        pmxl->dwType = 0;
        pmxl->dwLineID = 1;
    }
    return MMSYSERR_NOERROR;
}

MMRESULT mixerGetLineControlsA(HMIXER hmxobj, LPMIXERLINECONTROLS pmxlc,
                               DWORD fdwControls) {
    (void)hmxobj; (void)fdwControls;
    if (!pmxlc || !pmxlc->pamxctrl) return MMSYSERR_INVALPARAM;

    LPMIXERCONTROL ctrl = pmxlc->pamxctrl;
    if (pmxlc->cControls >= 1) {
        memset(&ctrl[0], 0, sizeof(MIXERCONTROL));
        ctrl[0].cbStruct = sizeof(MIXERCONTROL);
        ctrl[0].dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
        ctrl[0].cChannels = 2;
        ctrl[0].dwMinimum = 0;
        ctrl[0].dwMaximum = 0xFFFF;
        ctrl[0].cSteps = 101;
        strncpy(ctrl[0].szShortName, "Vol", 16);
        strncpy(ctrl[0].szName, "Volume", 64);
    }
    if (pmxlc->cControls >= 2) {
        memset(&ctrl[1], 0, sizeof(MIXERCONTROL));
        ctrl[1].cbStruct = sizeof(MIXERCONTROL);
        ctrl[1].dwControlType = MIXERCONTROL_CONTROLTYPE_MUTE;
        ctrl[1].cChannels = 2;
        ctrl[1].dwMinimum = 0;
        ctrl[1].dwMaximum = 1;
        ctrl[1].cSteps = 2;
        strncpy(ctrl[1].szShortName, "Mute", 16);
        strncpy(ctrl[1].szName, "Mute", 64);
    }
    return MMSYSERR_NOERROR;
}

MMRESULT mixerSetControlDetails(HMIXER hmxobj, LPMIXERCONTROLDETAILS pmxcd,
                                DWORD fdwDetails) {
    (void)hmxobj; (void)fdwDetails;
    if (!pmxcd) return MMSYSERR_INVALPARAM;

    // Volume control
    // Use dwValue1 as a pointer to MIXERCONTROLDETAILS_UNSIGNED
    LPMIXERCONTROLDETAILS_UNSIGNED pval =
        (LPMIXERCONTROLDETAILS_UNSIGNED)(pmxcd->dwValue1);
    if (pval) {
        int vol = (int)pval->dwValue;
        vol = vol * 100 / 0xFFFF;
        audio_set_volume(vol);
    }
    return MMSYSERR_NOERROR;
}

MMRESULT mixerGetControlDetailsA(HMIXER hmxobj, LPMIXERCONTROLDETAILS pmxcd,
                                 DWORD fdwDetails) {
    (void)hmxobj; (void)fdwDetails;
    if (!pmxcd) return MMSYSERR_INVALPARAM;

    LPMIXERCONTROLDETAILS_UNSIGNED pval =
        (LPMIXERCONTROLDETAILS_UNSIGNED)(pmxcd->dwValue1);
    if (pval) {
        int vol = audio_get_volume();
        pval->dwValue = (DWORD)(vol * 0xFFFF / 100);
    }
    return MMSYSERR_NOERROR;
}

// ═══════════════════════════════════════════════════════════════
// Multimedia Timer — timeGetTime()
// ═══════════════════════════════════════════════════════════════

DWORD timeGetTime(void) {
    return timer_ms();
}

MMRESULT timeBeginPeriod(UINT uPeriod) {
    (void)uPeriod;  // PIT tick is fixed at 50 Hz (20 ms)
    return MMSYSERR_NOERROR;
}

MMRESULT timeEndPeriod(UINT uPeriod) {
    (void)uPeriod;
    return MMSYSERR_NOERROR;
}

// ═══════════════════════════════════════════════════════════════
// waveOut service — called from audio_tick() to drain waveOut rings
// into the AC97 DMA engine.
// ═══════════════════════════════════════════════════════════════

void wasapi_service(void) {
    for (int i = 0; i < WAVE_MAX_HANDLES; i++) {
        wave_handle_t* h = &wave_out_handles[i];
        if (!h->in_use || h->paused) continue;
        if (h->pcm_available == 0) continue;
        wave_out_service_ring(h);
    }
}
