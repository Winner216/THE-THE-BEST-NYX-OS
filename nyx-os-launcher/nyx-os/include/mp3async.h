// include/mp3async.h — Background MP3 decode worker for Nyx OS
//
// Decodes the MP3 elementary stream (muxed into the MP4 by
// tools/transcode-videos.py) in a SEPARATE SCHEDULED TASK so the
// video player's UI/decode loop never has to pay the IMDCT +
// filterbank cost synchronously.  The worker task reads the stream
// (read-only), decodes frame by frame with the integer MP3 decoder
// (cpu/mp3dec.c), and pushes the resulting 44.1 kHz STEREO int16 PCM
// into an SPSC ring.
//
// Ownership (same single-writer-per-phase discipline as jpegasync):
//   • MAIN TASK writes the stream fields + state flag, and posts seek
//     requests.  Never touches the ring data.
//   • WORKER TASK reads the stream, decodes, and writes ring samples
//     + the write cursor.  Never reads the read cursor past space
//     checks.
//   • IRQ side (audio_tick) is the ring CONSUMER: it reads samples
//     and advances the read cursor.  It never writes worker fields.
//
// SAFETY CONTRACT: the worker only reads the caller's MP3 bytes and
// writes its own static state + the ring, so it is safe to preempt at
// ring 0 (mirrors jpegasync).  The scheduler API must not be called
// from an IRQ, so the worker task is created lazily from the main
// loop (mp3_async_ensure_worker), never from a start/seek call.

#ifndef MP3ASYNC_H
#define MP3ASYNC_H

#include <stdint.h>

// Decoded PCM ring: STEREO int16 interleaved at the MP3's native
// rate (44.1 kHz).  Power-of-two size 2^15 frames = ~0.74 s of
// audio — the worker fills it aggressively (decode back-to-back
// while space allows) and sleeps once the cushion is topped up, so
// the ring stays full and decode bursts (a slow TCG host) never
// starve the AC'97 DMA.  Small enough to bound memory (128 KiB) and
// keep the decode-ahead horizon short, large enough to absorb any
// realistic scheduler stall.  The AC'97 preload depth (~80 ms), not
// this ring, sets playback latency.
#define MP3A_RING_FRAMES 32768
#define MP3A_RING_MASK   (MP3A_RING_FRAMES - 1)

// Start decoding `stream` (an MPEG1 Layer III elementary stream) in
// the background.  Must be called from the MAIN LOOP (creates the
// worker task).  A second start while running re-attaches a new
// stream from the beginning.
void mp3_async_start(const uint8_t* stream, int len);

// Stop decoding and reset the ring.  Safe from main-loop context.
void mp3_async_stop(void);

// Request a seek: the worker lands on the MP3 frame nearest `ms`,
// flushes the ring, and resumes from there.  Main-loop only.
void mp3_async_seek_ms(int ms);

// Ensure the worker task exists (main loop only, mirrors
// jpeg_async_ensure_worker).  Called by mp3_async_start internally.
void mp3_async_ensure_worker(void);

// ─── IRQ-side ring consumer API (audio_tick only) ────────────────
int  mp3_async_available(void);                 // stereo frames readable now
int  mp3_async_read(int16_t* out, int maxf);    // copy up to maxf frames
int  mp3_async_eos(void);                       // worker hit end of stream
int  mp3_async_elapsed_ms(void);                // playback position (consumed)

// Presentation Time Stamp of the next PCM block, in µs — the master
// clock's AUDIO reference (cpu/video.c).  Every decoded block has a
// PTS: base_ms anchor + consumed source samples at the stream rate.
int64_t mp3_async_pts_us(void);

#endif
