// include/timer.h — High-resolution system clock (TSC) for Nyx OS
//
// The PIT IRQ gives 20 ms tick granularity — far too coarse for
// audio/video sync.  This module reads the CPU time-stamp counter
// (RDTSC, a 64-bit cycle counter) and calibrates its frequency
// against the PIT once at boot, so timer_us()/timer_ms() track
// wall-clock time down to microsecond resolution.
//
// The TSC clock is the kernel's high-resolution time base.  The
// video player uses it two ways:
//   • as the MASTER CLOCK when no audio is playing (video clock);
//     when sound is present, the audio sample counter is master and
//     the playhead follows it (see cpu/video.c vd_master_us()).
//   • to attach PRESENTATION TIME STAMPS (PTS): every decoded video
//     frame gets vd_frame_pts_us(), every MP3 block gets
//     mp3_async_pts_us(), and the master clock is the single
//     timeline both are compared against.
//
// If RDTSC is unavailable or the calibration is implausible, the
// clock silently falls back to the PIT tick counter (~1 ms).

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

// Calibrate the TSC against the PIT.  Called once at boot, after the
// PIT IRQ is live (needs `tick` to advance).  Idempotent.
void timer_init(void);

// Microseconds since boot.  TSC-based (sub-µs jitter, calibrated
// frequency) or PIT fallback (~1 ms granularity) if TSC is broken.
uint64_t timer_us(void);

// Milliseconds since boot — convenience wrapper over timer_us().
uint32_t timer_ms(void);

// 1 once the TSC clock is live and calibrated (never 0 after a
// successful timer_init; 0 only in the PIT fallback path).
int timer_ready(void);

#endif
