#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

extern int sound_volume;

void speaker_play(uint32_t frequency);
void speaker_stop();
void speaker_beep();
void speaker_beep_high();
void speaker_beep_low();
void speaker_tick();
void speaker_set_volume(int vol);
void delay(uint32_t ms);

// ─── Chord-faking (PC-speaker is a single oscillator, so "chords"
//     are an illusion built from extremely fast tone-switching) ───
// Called once per timer tick (≈18.2Hz) while a note with harmony
// tones is sounding. Does a SHORT, bounded burst of frequency swaps
// (a few hundred microseconds total) so it never stalls the IRQ,
// while still being fast enough for the ear to blend the tones.
void speaker_chord_tick(uint32_t f0, uint32_t f1, uint32_t f2, int style, uint32_t phase);

#endif
