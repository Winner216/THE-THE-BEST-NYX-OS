#ifndef GAMESND_H
#define GAMESND_H

#include <stdint.h>

// ─── Game sound effects (ring-3 → kernel mixer → AC'97) ─────────
// A ring-3 game (DOOM.EXE) submits raw 8-bit unsigned mono PCM via
// SYS_SNDLOAD — copied into the kernel heap inside the syscall (user
// pages are only mapped while the submitting task owns the CR3, so
// the copy must never happen in IRQ context) — and triggers voices
// with SYS_SNDPLAY(handle, vol, sep, chan).
//
// audio_tick() (PIT IRQ, 50 Hz) mixes all active voices into one
// 48 kHz stereo slot and pushes it through the AC'97 DMA path,
// taking priority over the video/MP3 source while any voice is live
// (plus a drain hold so the handoff back doesn't click).
//
// Cleanup: gamesnd_task_exit(pid) stops the task's voices and frees
// its cached PCM; call it wherever a task's PCB is freed.

// Returns a handle ≥ 0, -2 if AC'97 is absent (lets the game's sound
// module Init() fail cleanly), or -1 if args invalid / the 1 MiB
// per-task cache is full.  pcm is 8-bit unsigned mono.
int  gamesnd_load(int pid, const uint8_t* pcm, int nbytes, int rate);

// Trigger (or retrigger) a voice.  vol 0-127, sep 0-254 with 128 =
// centre (DOOM's NORM_SEP), chan 0-15 is the game's own channel id —
// re-playing a (pid, chan) replaces that voice.
int  gamesnd_play(int pid, int handle, int vol, int sep, int chan);

// Stop the voice the task owns on `chan`.
int  gamesnd_stop(int pid, int chan);

// Live volume/pan update while a voice plays.
int  gamesnd_params(int pid, int chan, int vol, int sep);

// 1 if the task's voice on `chan` is still sounding.
int  gamesnd_isplaying(int pid, int chan);

// Stop + free everything the task owns (call when the PCB is freed).
void gamesnd_task_exit(int pid);

// ─── audio_tick integration (cpu/audio.c) ───────────────────────
int  gamesnd_voices_active(void);       // voices currently sounding
// Mix one 20 ms slot of 48 kHz stereo into `out` (AC97_SLOT_FRAMES
// frames × 2 samples, smoothed system volume applied).  Voices that
// finish mid-slot are retired.
void gamesnd_render_slot(int16_t* out);

#endif // GAMESND_H
