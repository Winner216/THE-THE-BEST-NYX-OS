// cpu/mp3async.c — Background MP3 decode worker task + SPSC PCM ring
//
// Decodes an MP3 elementary stream (muxed into the MP4 by
// tools/transcode-videos.py) in a SEPARATE SCHEDULED TASK so the
// video player never pays the IMDCT + filterbank cost synchronously.
// The worker task reads the stream read-only, decodes frame by frame
// with the integer decoder (cpu/mp3dec.c), and pushes 44.1 kHz
// STEREO int16 PCM into an SPSC ring.  The IRQ-side consumer
// (audio_tick in cpu/audio.c) drains the ring and feeds the AC'97
// DMA.
//
// Ownership (single-writer-per-phase, like jpegasync):
//   main  →  w_state, w_stream, w_len, seek request
//   worker→  ring samples + w_wr (write cursor), w_eos
//   IRQ   →  ring reads + w_rd (read cursor)
// No locks; every shared field is volatile with one writer per phase.

#include <mp3async.h>
#include <mp3dec.h>
#include <task.h>
#include <io.h>
#include <stdint.h>
#include <stddef.h>

// COM1 probe log (invisible on the VESA screen, visible in QEMU's
// serial log — same trick isr.c/ac97.c use).  TEMPORARY debug.
static void mp3a_serial(const char* s) {
    for (; *s; s++) {
        int spins = 0;
        while ((inb(0x3F8 + 5) & 0x20) == 0 && spins++ < 200000) { }
        outb(0x3F8, (uint8_t)*s);
    }
}
static void mp3a_serial_num(uint32_t v) {
    char tmp[12]; int i = 0;
    if (v == 0) { mp3a_serial("0"); return; }
    while (v > 0) { tmp[i++] = '0' + v % 10; v /= 10; }
    while (i > 0) mp3a_serial(&tmp[--i]);
}
static void mp3a_log(const char* tag, uint32_t a, uint32_t b) {
    mp3a_serial(tag); mp3a_serial(" "); mp3a_serial_num(a);
    mp3a_serial(" "); mp3a_serial_num(b); mp3a_serial("\n");
}

typedef enum {
    W_IDLE    = 0,
    W_RUNNING = 1,
} w_state_t;

static volatile w_state_t     w_state;
static volatile const uint8_t* w_stream;      // MP3 elementary stream
static volatile int           w_len;
static volatile int           w_seek_req;    // 1 = seek pending
static volatile int           w_seek_ms;     // target playback position
static volatile int           w_eos;         // worker hit end of stream
static int                    w_started;     // worker task created

// ─── SPSC ring (worker writes, IRQ reads) ────────────────────────
static int16_t    ring[MP3A_RING_FRAMES * 2] __attribute__((aligned(64)));
static volatile uint32_t w_wr;    // next write slot (worker only)
static volatile uint32_t w_rd;    // next read slot  (IRQ only)
// Absolute source frames consumed — for playback position.  Written
// only by the IRQ side (w_rd advances 1:1 with it), so a plain
// volatile read from the main loop is race-free enough for display.
static volatile uint32_t w_consumed;
// Playback position at the last seek point, in ms.  Written by the
// WORKER when it lands a seek, read by the IRQ/main loop, so it
// joins the consumed counter as: elapsed = w_base_ms + consumed*1000/44100.
static volatile int w_base_ms;

// The decoder context lives INSIDE the worker (private static) so it
// survives the worker's preemption — the decode state (overlap, QMF
// history, bit reservoir) is not shared with anyone else.
static Mp3Dec  w_dec;

// Frame index for seeks: byte offsets of every MP3 frame in the
// stream, walked once at attach time with the cheap header parser
// (no decode).  Lets the worker jump to the frame nearest a target
// ms without decoding from the start.
#define W_MAX_FRAMES 4096
static uint32_t w_frm_off[W_MAX_FRAMES];
static int      w_frm_count;
static int      w_samplerate;   // from the first frame header (44.1 kHz)

// Build the frame-offset index.  Returns the first frame offset or -1.
static int w_build_index(void) {
    w_frm_count = 0;
    const uint8_t* d = (const uint8_t*)w_stream;
    int len = w_len;
    int off = 0;
    if (len >= 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
        int sz = (d[6] << 21) | (d[7] << 14) | (d[8] << 7) | d[9];
        off = 10 + sz;
        if (d[5] & 0x10) off += 10;
    }
    // Scan for the first sync, then walk frame by frame.
    int fb;
    int first = -1;
    while (off + 4 <= len) {
        if (d[off] == 0xFF && (d[off+1] & 0xE0) == 0xE0) {
            fb = mp3_header_frame_bytes(d, len, off);
            if (fb >= 4 && off + fb <= len) {
                first = off;
                break;
            }
        }
        off++;
    }
    if (first < 0) return -1;
    while (off + 4 <= len && w_frm_count < W_MAX_FRAMES) {
        fb = mp3_header_frame_bytes(d, len, off);
        if (fb < 4 || off + fb > len) break;
        w_frm_off[w_frm_count++] = (uint32_t)off;
        off += fb;
    }
    // Sample rate from the first frame header.
    w_samplerate = 44100;
    Mp3Dec tmp;
    if (mp3_init(&tmp, d + first, len - first) >= 0)
        w_samplerate = tmp.samplerate;
    return first;
}

// Re-anchor the decoder at the frame nearest `ms` (or the start).
static void w_do_seek(void) {
    int ms = w_seek_ms; if (ms < 0) ms = 0;
    int fi = 0;
    int frame_ms = 0;
    if (w_samplerate > 0 && w_frm_count > 0) {
        // each frame = 1152 samples
        frame_ms = 1152 * 1000 / w_samplerate;
        if (frame_ms > 0) {
            fi = ms / frame_ms;
            if (fi >= w_frm_count) fi = w_frm_count - 1;
        }
    }
    const uint8_t* base = (const uint8_t*)w_stream;
    int len = w_len;
    int off = (int)w_frm_off[fi];
    // Fresh decoder state at this frame (bit reservoir starts empty —
    // the first frame or two after a seek may drop, standard MP3
    // behavior, but the stream is never corrupted).
    if (mp3_init(&w_dec, base + off, len - off) >= 0) {
        // The decoder is positioned; clear the ring and re-anchor the
        // read cursor.  Ring is reset from the WORKER side (only the
        // worker may touch ring data), the consumer sees rd == wr.
        w_rd = 0;
        w_wr = 0;
        w_consumed = 0;
        w_base_ms = (frame_ms > 0) ? fi * frame_ms : 0;
        w_eos = 0;
    }
    w_seek_req = 0;
}

// The worker task: decode frames back-to-back while the ring has
// room, sleep only when it can't (ring full, EOS, or not running).
// Filling the ring aggressively means decode bursts never starve the
// AC'97 DMA even on a slow TCG host, and the worker hands the CPU
// back as soon as the cushion is topped up.  The RR timeslice bounds
// each burst; only touches its own decoder, the ring and w_wr.
static void mp3_worker_entry(void) {
    for (;;) {
        if (w_state == W_RUNNING) {
            if (w_seek_req) w_do_seek();
            // Space available?  Keep one slot free so full/empty are
            // distinguishable (wr == rd means empty).
            uint32_t rd = w_rd;
            uint32_t wr = w_wr;
            uint32_t used = (wr - rd) & MP3A_RING_MASK;
            if (used + MP3_FRAME_SAMPLES < MP3A_RING_FRAMES) {
                int n = mp3_decode_frame(&w_dec);
                if (n == MP3_FRAME_SAMPLES) {
                    // interleave the two channels into the ring
                    for (int i = 0; i < MP3_FRAME_SAMPLES; i++) {
                        ring[((wr + i) & MP3A_RING_MASK) * 2]     = w_dec.pcm[0][i];
                        ring[((wr + i) & MP3A_RING_MASK) * 2 + 1] = w_dec.pcm[1][i];
                    }
                    w_wr = (wr + MP3_FRAME_SAMPLES) & MP3A_RING_MASK;
                    w_eos = 0;
                    mp3a_log("MP3A frm", (uint32_t)(w_frm_count > 0 ? 1 : 0), 0);
                    continue;   // keep decoding while the ring has room
                }
                w_eos = 1;   // end of stream (or corrupt frame)
                mp3a_log("MP3A eos", 0, 0);
            }
        }
        task_sleep(1);
    }
}

// ─── Public API (main loop) ─────────────────────────────────────
void mp3_async_ensure_worker(void) {
    if (w_started) return;
    int id = task_create("mp3dec", mp3_worker_entry);
    if (id >= 0) {
        w_started = 1;
        // Audio decode is the high-priority background thread: under
        // SCHED_PRIORITY it preempts UI tasks so the PCM ring never
        // starves; under the default SCHED_RR it's a no-op.  The
        // worker sleeps as soon as the ring is full, so the elevated
        // priority costs the UI nothing in practice.
        task_set_priority(id, 220);
    } else {
        w_state = W_IDLE;   // no task table room — stay silent
    }
}

void mp3_async_start(const uint8_t* stream, int len) {
    mp3_async_ensure_worker();
    w_stream = stream;
    w_len = len;
    w_eos = 0;
    w_rd = 0;
    w_wr = 0;
    w_consumed = 0;
    w_base_ms = 0;
    w_seek_req = 0;
    if (w_build_index() >= 0) {
        w_state = W_RUNNING;
        // Position the decoder at the first frame synchronously so the
        // worker's first wakeup has valid state.
        w_seek_ms = 0;
        w_do_seek();
    } else {
        w_state = W_IDLE;   // not a decodable MPEG1 stream
    }
    mp3a_log("MP3A start", (uint32_t)len, (uint32_t)w_frm_count);
}

void mp3_async_stop(void) {
    w_state = W_IDLE;
    w_stream = NULL;
    w_len = 0;
    w_eos = 0;
    w_rd = 0;
    w_wr = 0;
    w_consumed = 0;
    w_base_ms = 0;
    w_seek_req = 0;
}

void mp3_async_seek_ms(int ms) {
    if (w_state != W_RUNNING) return;
    w_seek_ms = ms;
    w_seek_req = 1;   // worker picks it up on its next wakeup
}

// ─── IRQ-side consumer (audio_tick) ──────────────────────────────
int mp3_async_available(void) {
    return (int)((w_wr - w_rd) & MP3A_RING_MASK);
}

int mp3_async_read(int16_t* out, int maxf) {
    uint32_t rd = w_rd;
    int avail = (int)((w_wr - rd) & MP3A_RING_MASK);
    if (avail > maxf) avail = maxf;
    for (int i = 0; i < avail; i++) {
        int idx = ((rd + i) & MP3A_RING_MASK) * 2;
        out[i * 2]     = ring[idx];
        out[i * 2 + 1] = ring[idx + 1];
    }
    w_rd = (rd + avail) & MP3A_RING_MASK;
    w_consumed += (uint32_t)avail;
    return avail;
}

int mp3_async_eos(void) {
    return w_eos;
}

int mp3_async_elapsed_ms(void) {
    return w_base_ms + (int)((uint64_t)w_consumed * 1000 / 44100);
}

// ─── Audio PTS ───────────────────────────────────────────────────
// The PRESENTATION TIME STAMP of the next PCM block the IRQ will
// consume: wall-clock µs since the stream began (base_ms anchor +
// consumed source samples at the stream's sample rate).  Every
// decoded block has a PTS, so the master clock in cpu/video.c can
// compare audio position directly against per-frame video PTS on
// the same µs timeline.
int64_t mp3_async_pts_us(void) {
    int sr = w_samplerate > 0 ? w_samplerate : 44100;
    return (int64_t)w_base_ms * 1000
         + (int64_t)((uint64_t)w_consumed * 1000000 / sr);
}
