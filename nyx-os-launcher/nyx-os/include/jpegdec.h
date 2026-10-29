// include/jpegdec.h — Minimal JPEG decoder for Nyx OS
#ifndef JPEGDEC_H
#define JPEGDEC_H

#include <stdint.h>

// Decoded image info
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t* pixels;   // 32-bit RGBA
    int      stride;   // byte stride per output row; 0 = tightly packed (width*4)
} JpegImage;

// Decode a JPEG from raw file data (baseline only).
// Returns 1 on success, 0 on failure.
// The caller must provide a pixel buffer of at least width*height*4 bytes.
// Set img->stride to the destination's row stride in bytes to decode
// directly into a non-packed buffer (e.g. the framebuffer at vesa.pitch
// — the zero-copy path in cpu/video.c); 0 = tightly packed.  Callers
// must zero the struct before use.
int jpeg_decode(const uint8_t* data, int datalen, JpegImage* img);

// ── Internal ─────────────────────────────────────────────────────
// Shared decode core (mode 0 = full, 1 = vertical-half, 2 = both-axes
// ECO).  Exported for the async worker (cpu/jpegasync.c); ordinary
// callers should use jpeg_decode / jpeg_decode_eco_v / jpeg_decode_eco.
int jpeg_decode_inner(const uint8_t* data, int datalen,
                      JpegImage* img, int mode);

// ── Async decode (background worker task) ────────────────────────
// The kernel is a single-CPU OS, so the decode is not run on a second
// core — it runs in a separate preemptive task, and the 50 Hz PIT
// scheduler time-slices between the worker and the desktop.  A large
// image therefore no longer monopolizes the CPU and freezes the UI
// thread while it decodes.  The caller's pixel buffer is written by
// the worker while the main task is idle — do not touch `out` until
// jpeg_async_done() returns true.
//
// Submit a decode job (data/datalen/out/stride/mode — see
// jpeg_decode_eco_* for mode semantics).  Returns 1 if accepted, 0 if
// a job is already running (the caller should retry or ignore).
int  jpeg_async_submit(const uint8_t* data, int datalen,
                       uint8_t* out, int stride, int mode);
// Main-loop only: lazily creates the worker task (must never run in
// IRQ context).  Call once per poll cycle before checking jpeg_async_done.
void jpeg_async_ensure_worker(void);
// Marks the finished job consumed (back to idle) so the next submit is
// accepted.  Call with interrupts disabled after reading the result.
void jpeg_async_reset(void);
// 1 once the submitted job has finished (success or failure).
int  jpeg_async_done(void);
// Result of the finished job: 1 = decoded ok, 0 = decode failed.
int  jpeg_async_ok(void);
// Output dimensions of the finished job (valid after jpeg_async_done).
uint32_t jpeg_async_width(void);
uint32_t jpeg_async_height(void);

// Decode at HALF VERTICAL resolution (width × height/2) for adaptive
// playback — the intermediate rung of the video player's ECO ladder.
// Only every other MCU ROW is rendered (all columns kept, so full
// horizontal detail), cutting decode time ~2× on large frames; the
// entropy stream is still consumed in full so DC prediction stays in
// sync.  Output dims are set on img; the caller's buffer must hold at
// least full width*height*4 bytes.  Returns 1 on success, 0 on failure.
int jpeg_decode_eco_v(const uint8_t* data, int datalen, JpegImage* img);

// Decode at QUARTER resolution (width/2 × height/2) for adaptive
// playback (the video player's last-resort ECO fallback).  Only every
// other MCU row/column is rendered, cutting decode time ~3-4× on large
// frames; the entropy stream is still consumed in full so DC prediction
// stays in sync.  Output dims are set on img; the caller's buffer must
// hold at least full width*height*4 bytes (eco writes into the top-left
// quarter-sized region).  Returns 1 on success, 0 on failure.
int jpeg_decode_eco(const uint8_t* data, int datalen, JpegImage* img);

// Light deblocking filter for JPEG 8×8 MCU grid artifacts.  A
// simplified H.264-style weak filter applied ONLY at the 8-pixel block
// boundaries on both axes (block interiors are never touched).  Luma
// (r+g+b) drives both the decision and the delta; the same delta is
// applied to all three channels so hue is preserved.  In-place on an
// RGBA buffer; stride is the byte row stride (0 = tightly packed
// width*4).  Intended for video frames after decode, before scaling.
void jpeg_deblock_rgba(uint8_t* rgba, int w, int h, int stride);

#endif
