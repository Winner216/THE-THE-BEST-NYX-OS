// cpu/timer.c — TSC high-resolution clock, calibrated against the PIT
//
// The PIT channel-0 countdown gives sub-tick precision (the same
// trick video.c uses for decode timing): latch the countdown, read
// the TSC, wait a few PIT periods, latch again.  The elapsed PIT
// counts are exact (tick delta × reload divisor + fractional term),
// so the TSC frequency is measured to well under 1% in ~60 ms — good
// enough for µs wall-clock reads.
//
// RDTSC is not serializing, so the read is prefixed with a cpuid
// fence — otherwise the instruction stream can reorder the read
// ahead of the code that precedes it, skewing the timestamp by tens
// of cycles.  (A `cpuid` costs ~100 cycles; timer_us() is called at
// most a few times per heartbeat, so the cost is irrelevant.)

#include <timer.h>
#include <isr.h>        // tick, PIT_BASE_HZ, PIT_DIVISOR
#include <io.h>         // inb/outb
#include <stdint.h>

static int      t_tsc_ok   = 0;
static uint64_t t_tsc_hz   = 0;    // calibrated cycles per second
static uint64_t t_base     = 0;    // TSC value at calibration time

static inline uint64_t tsc_read(void) {
    uint32_t lo, hi;
    __asm__ volatile("cpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Latch the PIT channel-0 countdown (mode 3: decrements by 2/clock).
static uint32_t pit_countdown(void) {
    outb(0x43, 0x00);              // latch channel 0
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint32_t)((hi << 8) | lo);
}

void timer_init(void) {
    // Sample TSC across 3 PIT periods (~60 ms) with countdown
    // fraction on both edges.  Interrupts are enabled (the PIT IRQ
    // advances `tick`), so IRQ latency just adds a bounded error to
    // the measured interval — the ratio is still accurate.
    uint32_t t0 = tick;
    uint32_t c0 = pit_countdown();
    uint64_t s0 = tsc_read();

    while ((uint32_t)(tick - t0) < 3) { /* ~60 ms */ }

    uint32_t t1 = tick;
    uint32_t c1 = pit_countdown();
    uint64_t s1 = tsc_read();

    uint32_t r0 = (c0 & 0xFFFF); if (r0 > PIT_DIVISOR) r0 = PIT_DIVISOR;
    uint32_t r1 = (c1 & 0xFFFF); if (r1 > PIT_DIVISOR) r1 = PIT_DIVISOR;
    uint32_t since0 = PIT_DIVISOR - r0;
    uint32_t since1 = PIT_DIVISOR - r1;
    // Cast before subtract (the video.c lesson): the fraction can
    // wrap mid-period, so the signed difference must be computed on
    // int64 operands, not in unsigned 32-bit.
    int64_t counts = (int64_t)(t1 - t0) * PIT_DIVISOR
                   + ((int64_t)since1 - (int64_t)since0) * 2;
    if (counts <= 0) return;       // no PIT movement — keep fallback
    int64_t elapsed_us = counts * 1000000 / PIT_BASE_HZ;
    if (elapsed_us <= 0) return;

    uint64_t hz = (uint64_t)((s1 - s0) * 1000000 / elapsed_us);
    // Sanity: a real x86 TSC is 100 MHz..10 GHz.  Outside that, the
    // measurement is garbage (e.g. no RDTSC support emulating 0).
    if (hz < 100000000ull || hz > 10000000000ull) return;

    t_tsc_hz = hz;
    t_base   = s1;                 // zero the clock at calibration
    t_tsc_ok = 1;
}

int timer_ready(void) { return t_tsc_ok; }

uint64_t timer_us(void) {
    if (!t_tsc_ok) {               // PIT fallback: ~1 ms resolution
        return (uint64_t)tick * 1000000 / PIT_TARGET_HZ;
    }
    uint64_t delta = tsc_read() - t_base;
    // (delta * 1000000) can overflow 64-bit after ~5000 s of uptime,
    // so divide first: delta / (hz/1e6) keeps sub-µs rounding error
    // and cannot overflow.  hz/1e6 is 100..10000, exact per-µs rate.
    return delta / (t_tsc_hz / 1000000ull);
}

uint32_t timer_ms(void) {
    return (uint32_t)(timer_us() / 1000);
}
