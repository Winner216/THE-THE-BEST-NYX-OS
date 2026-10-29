// cpu/speaker.c
#include <speaker.h>
#include <io.h>
#include <stdint.h>

#define PIT_BASE_FREQ 1193180

static volatile uint32_t beep_ticks = 0;

// Raw PIT channel-2 frequency set, shared by every public entry point
// below so the chord engine and the plain single-tone path always
// agree on exactly how a frequency maps to a PIT divisor.
static void pit_set_freq(uint32_t frequency) {
    uint32_t scaled = frequency * sound_volume / 100;
    if (scaled < 100) scaled = 100;          // floor so divisor never explodes
    uint32_t divisor = PIT_BASE_FREQ / scaled;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)(divisor >> 8));
}

// Internal play that respects volume
// If volume is 0 — completely silent
static void play_freq(uint32_t frequency, uint32_t ticks) {
    if (sound_volume == 0) return; // Muted!
    pit_set_freq(frequency);
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp | 0x03);
    beep_ticks = ticks;
}

void speaker_play(uint32_t frequency) {
    if (sound_volume == 0) return;
    pit_set_freq(frequency);
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp | 0x03);
}

void speaker_stop() {
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp & 0xFC);
    beep_ticks = 0;
}

void speaker_tick() {
    if (beep_ticks > 0) {
        beep_ticks--;
        if (beep_ticks == 0)
            speaker_stop();
    }
}

void speaker_beep() {
    play_freq(800, 3);
}

void speaker_beep_high() {
    play_freq(1200, 2);
}

void speaker_beep_low() {
    play_freq(400, 2);
}

void delay(uint32_t ms) {
    for (uint32_t i = 0; i < ms * 100000; i++)
        asm volatile("nop");
}

void speaker_set_volume(int vol) {
    sound_volume = vol;
}

// ─── Chord-faking engine ──────────────────────────────────────────
//
// The PC speaker (PIT channel 2 → port 0x61) is a single square-wave
// oscillator: it can only ever sound one frequency at an instant.
// Real "chords" on this hardware are an audio illusion built by
// switching frequencies fast enough that the ear's integration time
// (roughly 20-30ms) blends the separate tones into one perceived
// sound — the same trick used by PC speaker trackers and games like
// Wolfenstein 3D in the DOS era.
//
// This must run from inside the 18.2Hz timer IRQ (music_tick is
// called there), so it can NEVER busy-wait for anywhere close to a
// full tick (~55ms) or it would freeze keyboard/mouse/screen input
// system-wide. Instead it does a short, bounded burst of a few
// switches — enough to add real harmonic texture, while finishing
// in well under a millisecond.

// Microsecond-scale busy-wait, deliberately much finer-grained than
// the millisecond delay() above, so several switches fit in one tick.
static inline void usleep_short(uint32_t us) {
    // Calibrated empirically against this build's -O2 nop loop;
    // intentionally approximate — audio texture, not timing-critical.
    for (uint32_t i = 0; i < us * 100; i++)
        asm volatile("nop");
}

void speaker_chord_tick(uint32_t f0, uint32_t f1, uint32_t f2,
                         int style, uint32_t phase) {
    if (sound_volume == 0) return;
    if (f0 == 0) { speaker_stop(); return; }

    uint8_t tmp = inb(0x61);
    outb(0x61, tmp | 0x03);   // ensure speaker gate + data are enabled

    switch (style) {
        case 1: { // PLAY_CHORD — fast alternation across all present
                   // tones; several switches per tick so each tone
                   // gets airtime before the ear's window closes.
            uint32_t tones[3]; int n = 0;
            tones[n++] = f0;
            if (f1) tones[n++] = f1;
            if (f2) tones[n++] = f2;
            // 6 micro-slices per tick keeps total burst well under
            // 1ms while cycling every active tone at least once
            // (twice, for a 2-tone chord; three times for a single
            // root with no harmony, which just sounds plain).
            for (int s = 0; s < 6; s++) {
                pit_set_freq(tones[s % n]);
                usleep_short(40);
            }
            break;
        }
        case 2: { // PLAY_ARP — slower up/down sweep across the chord,
                   // one step per tick, so a held note arpeggiates
                   // root → third → fifth → third → root over its
                   // duration instead of blending into a single chord.
            uint32_t tones[3]; int n = 0;
            tones[n++] = f0;
            if (f1) tones[n++] = f1;
            if (f2) tones[n++] = f2;
            uint32_t steps = (uint32_t)(n > 1 ? n * 2 - 2 : 1);
            uint32_t idx;
            if (n == 1) idx = 0;
            else {
                uint32_t p = phase % steps;
                idx = (p < (uint32_t)n) ? p : steps - p;
            }
            pit_set_freq(tones[idx]);
            break;
        }
        case 3: { // PLAY_VIBRATO — tiny wobble around the root tone,
                   // makes a sustained note feel "alive" instead of
                   // a dead flat beep. Pure single-tone, no harmony.
            int32_t wob = (int32_t)(phase % 4) - 2;   // -2..1
            uint32_t f = f0 + (uint32_t)(wob * 3);
            pit_set_freq(f);
            break;
        }
        default: // PLAY_PLAIN
            pit_set_freq(f0);
            break;
    }
}
