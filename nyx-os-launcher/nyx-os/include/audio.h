#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

// ─── PCM player ──────────────────────────────────────────────────
// Plays raw 8-bit unsigned mono PCM (produced by tools/transcode-videos.py
// as a <name>.pcm sidecar at AUDIO_PCM_RATE samples/sec).
//
// Two output paths: when QEMU has the AC'97 sound card (cpu/ac97.c,
// real 16-bit stereo DMA playback at 48 kHz) that path is used — the
// 8-bit sidecar is resampled with 8x linear interpolation.  Without
// the card, the PC-speaker oscillator is used as a fallback: each
// sample becomes a tone whose pitch tracks the signal's amplitude
// deviation from silence (the classic DOS "robot voice").
//
// audio_tick() is called from the PIT IRQ (~50 Hz) and steps a
// bounded slice of samples per tick so it never stalls the
// mouse/keyboard handlers.

// Sidecar PCM sample rate — must match tools/transcode-videos.py.
#define AUDIO_PCM_RATE 6000

// Load PCM and start playing from pos_ms (0 = start).  Silently stays
// silent if pcm/len/rate are invalid (e.g. the video had no audio).
void audio_begin(const uint8_t* pcm, int len, int rate, int pos_ms);

// ─── MP3 source (in-container audio track) ───────────────────────
// Start playing from the MP3 elementary stream: the background worker
// (cpu/mp3async.c) decodes it at 44.1 kHz stereo and audio_tick
// resamples to 48 kHz for the AC'97 path.  Preferred over the 6 kHz
// .pcm sidecar when the MP4 carries a muxed track.  pos_ms re-anchors
// the worker at that position (resume after pause).
void audio_begin_mp3(const uint8_t* stream, int len, int pos_ms);
// Stop the MP3 source and its worker (video_close).
void audio_stop_mp3(void);

// Halt sound but keep the read position (for pause + resume).
void audio_stop(void);

// Jump the read position without changing play state.
void audio_set_pos_ms(int ms);

int audio_has_audio(void);      // valid PCM attached
int audio_is_playing(void);     // actively sounding
int audio_elapsed_ms(void);     // current read position in ms

// Audio PTS in µs — the master clock's AUDIO reference (cpu/video.c
// compares it against per-frame video PTS on one µs timeline).
int64_t audio_elapsed_us(void);

void audio_mute(int m);         // 1 = mute, 0 = unmute
int  audio_is_muted(void);

// ─── Volume + channel mixing ────────────────────────────────────
// Volume is applied per-sample in the AC'97 renderers through a
// smoothed 16.16 gain following the kernel-wide sound_volume knob
// (0-100).  audio_set_volume() just sets the knob; the renderers
// ease the gain to it over ~160 ms, so slider moves glide instead
// of stepping (the codec register only has 31 coarse steps).
void audio_set_volume(int v);   // 0-100, clamped
int  audio_get_volume(void);    // current knob value

// Downmix the decoded stereo pair to mono ((L+R)/2 on both
// channels) — useful for the PC-speaker fallback or mono sources.
void audio_set_mono(int m);     // 1 = mono, 0 = stereo
int  audio_is_mono(void);

// Called from the PIT IRQ (~50 Hz); steps rate/50 samples.
void audio_tick(void);

// ─── Shared smoothed-gain helpers (used by cpu/gamesnd.c too) ───
// Ease the 16.16 gain toward the sound_volume knob (call once per
// render pass) and apply gain + hard clip to one sample.
void    audio_gain_service(void);
int16_t audio_gain_apply(int32_t s);

#endif
