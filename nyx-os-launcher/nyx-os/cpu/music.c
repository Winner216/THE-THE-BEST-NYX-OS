// cpu/music.c
#include <music.h>
#include <speaker.h>
#include <io.h>
#include <stdint.h>

// ─── Song Definitions ─────────────────────────────────────────────

// Twinkle Twinkle Little Star
// Melody stays plain on quick passing notes; the long held notes at
// the end of each phrase get a I/IV/V chord underneath so the piece
// resolves with real harmony instead of a flat single tone.
static const Note twinkle[] = {
    N(NOTE_C4, DUR_QTR), N(NOTE_C4, DUR_QTR),
    N(NOTE_G4, DUR_QTR), N(NOTE_G4, DUR_QTR),
    N(NOTE_A4, DUR_QTR), N(NOTE_A4, DUR_QTR),
    CHORD(NOTE_G4, NOTE_C5, NOTE_E4, DUR_HALF),          /* G chord-ish resolve */
    N(NOTE_F4, DUR_QTR), N(NOTE_F4, DUR_QTR),
    N(NOTE_E4, DUR_QTR), N(NOTE_E4, DUR_QTR),
    N(NOTE_D4, DUR_QTR), N(NOTE_D4, DUR_QTR),
    CHORD(NOTE_C4, NOTE_E4, NOTE_G4, DUR_HALF),          /* C major — home */
    N(NOTE_G4, DUR_QTR), N(NOTE_G4, DUR_QTR),
    N(NOTE_F4, DUR_QTR), N(NOTE_F4, DUR_QTR),
    N(NOTE_E4, DUR_QTR), N(NOTE_E4, DUR_QTR),
    CHORD2(NOTE_D4, NOTE_G4, DUR_HALF),                  /* D/G suspended feel */
    N(NOTE_G4, DUR_QTR), N(NOTE_G4, DUR_QTR),
    N(NOTE_F4, DUR_QTR), N(NOTE_F4, DUR_QTR),
    N(NOTE_E4, DUR_QTR), N(NOTE_E4, DUR_QTR),
    CHORD2(NOTE_D4, NOTE_G4, DUR_HALF),
    N(NOTE_C4, DUR_QTR), N(NOTE_C4, DUR_QTR),
    N(NOTE_G4, DUR_QTR), N(NOTE_G4, DUR_QTR),
    N(NOTE_A4, DUR_QTR), N(NOTE_A4, DUR_QTR),
    CHORD(NOTE_G4, NOTE_C5, NOTE_E4, DUR_HALF),
    N(NOTE_F4, DUR_QTR), N(NOTE_F4, DUR_QTR),
    N(NOTE_E4, DUR_QTR), N(NOTE_E4, DUR_QTR),
    N(NOTE_D4, DUR_QTR), N(NOTE_D4, DUR_QTR),
    CHORD(NOTE_C4, NOTE_E4, NOTE_G4, DUR_WHOLE),         /* final cadence, held longer */
};

// Ode to Joy
// Beethoven's melody is iconic for the harmony underneath it as much
// as the tune itself, so this version chords almost every note —
// the fast alternation engine fakes a simple I-IV-V accompaniment.
static const Note ode_to_joy[] = {
    CHORD2(NOTE_E4, NOTE_C4, DUR_QTR), CHORD2(NOTE_E4, NOTE_C4, DUR_QTR),
    CHORD2(NOTE_F4, NOTE_C4, DUR_QTR), CHORD2(NOTE_G4, NOTE_C4, DUR_QTR),
    CHORD2(NOTE_G4, NOTE_C4, DUR_QTR), CHORD2(NOTE_F4, NOTE_C4, DUR_QTR),
    CHORD2(NOTE_E4, NOTE_C4, DUR_QTR), CHORD2(NOTE_D4, NOTE_G4, DUR_QTR),
    CHORD2(NOTE_C4, NOTE_E4, DUR_QTR), CHORD2(NOTE_C4, NOTE_E4, DUR_QTR),
    CHORD2(NOTE_D4, NOTE_G4, DUR_QTR), CHORD2(NOTE_E4, NOTE_C4, DUR_QTR),
    CHORD2(NOTE_E4, NOTE_C4, DUR_QTR), CHORD(NOTE_D4, NOTE_G4, NOTE_B4, DUR_HALF),
    REST(DUR_QTR),
    CHORD2(NOTE_E4, NOTE_C4, DUR_QTR), CHORD2(NOTE_E4, NOTE_C4, DUR_QTR),
    CHORD2(NOTE_F4, NOTE_C4, DUR_QTR), CHORD2(NOTE_G4, NOTE_C4, DUR_QTR),
    CHORD2(NOTE_G4, NOTE_C4, DUR_QTR), CHORD2(NOTE_F4, NOTE_C4, DUR_QTR),
    CHORD2(NOTE_E4, NOTE_C4, DUR_QTR), CHORD2(NOTE_D4, NOTE_G4, DUR_QTR),
    CHORD2(NOTE_C4, NOTE_E4, DUR_QTR), CHORD2(NOTE_C4, NOTE_E4, DUR_QTR),
    CHORD2(NOTE_D4, NOTE_G4, DUR_QTR), CHORD2(NOTE_E4, NOTE_C4, DUR_QTR),
    CHORD2(NOTE_D4, NOTE_G4, DUR_QTR), CHORD(NOTE_C4, NOTE_E4, NOTE_G4, DUR_HALF),
    REST(DUR_QTR),
};

// Happy Birthday
// Quick eighth-note pairs stay plain (chord-faking sounds cluttered
// on fast notes); the long held note ending each line gets a chord.
static const Note happy_birthday[] = {
    N(NOTE_C4, DUR_EIGHTH), N(NOTE_C4, DUR_EIGHTH),
    N(NOTE_D4, DUR_QTR),    N(NOTE_C4, DUR_QTR),
    N(NOTE_F4, DUR_QTR),    CHORD2(NOTE_E4, NOTE_C4, DUR_HALF),
    N(NOTE_C4, DUR_EIGHTH), N(NOTE_C4, DUR_EIGHTH),
    N(NOTE_D4, DUR_QTR),    N(NOTE_C4, DUR_QTR),
    N(NOTE_G4, DUR_QTR),    CHORD2(NOTE_F4, NOTE_C4, DUR_HALF),
    N(NOTE_C4, DUR_EIGHTH), N(NOTE_C4, DUR_EIGHTH),
    N(NOTE_C5, DUR_QTR),    N(NOTE_A4, DUR_QTR),
    N(NOTE_F4, DUR_QTR),    N(NOTE_E4, DUR_QTR),
    CHORD(NOTE_D4, NOTE_G4, NOTE_B4, DUR_HALF),
    N(NOTE_B4, DUR_EIGHTH), N(NOTE_B4, DUR_EIGHTH),
    N(NOTE_A4, DUR_QTR),    N(NOTE_F4, DUR_QTR),
    N(NOTE_G4, DUR_QTR),    CHORD(NOTE_F4, NOTE_C4, NOTE_A4, DUR_WHOLE),
};

// Imperial March (Darth Vader theme)
// The opening A4 half-notes are the most recognizable part of this
// theme — arpeggiating a minor-feel chord under them adds the
// ominous depth the real brass section provides, without touching
// the fast eighth-note runs where that would just sound muddy.
static const Note imperial_march[] = {
    N(NOTE_A4, DUR_QTR),  N(NOTE_A4, DUR_QTR),
    N(NOTE_A4, DUR_QTR),  N(NOTE_F4, DUR_EIGHTH),
    N(NOTE_C5, DUR_EIGHTH),N(NOTE_A4, DUR_QTR),
    N(NOTE_F4, DUR_EIGHTH),N(NOTE_C5, DUR_EIGHTH),
    ARP(NOTE_A4, NOTE_C5, NOTE_E5, DUR_HALF),
    N(NOTE_E5, DUR_QTR),  N(NOTE_E5, DUR_QTR),
    N(NOTE_E5, DUR_QTR),  N(NOTE_F5, DUR_EIGHTH),
    N(NOTE_C5, DUR_EIGHTH),N(NOTE_A4, DUR_QTR),
    N(NOTE_F4, DUR_EIGHTH),N(NOTE_C5, DUR_EIGHTH),
    ARP(NOTE_A4, NOTE_C5, NOTE_E5, DUR_HALF),
    N(NOTE_A5, DUR_QTR),  N(NOTE_A4, DUR_EIGHTH),
    N(NOTE_A4, DUR_EIGHTH),N(NOTE_A5, DUR_QTR),
    N(NOTE_G5, DUR_EIGHTH),N(NOTE_F5, DUR_EIGHTH),
    N(NOTE_E5, DUR_EIGHTH),N(NOTE_D5, DUR_EIGHTH),
    N(NOTE_E5, DUR_QTR),  REST(DUR_EIGHTH),
    N(NOTE_B4, DUR_QTR),  N(NOTE_G5, DUR_QTR),
    N(NOTE_F5, DUR_EIGHTH),N(NOTE_E5, DUR_EIGHTH),
    N(NOTE_D5, DUR_QTR),  N(NOTE_A5, DUR_QTR),
    N(NOTE_A4, DUR_EIGHTH),N(NOTE_A4, DUR_EIGHTH),
    N(NOTE_A5, DUR_QTR),  N(NOTE_G5, DUR_EIGHTH),
    N(NOTE_F5, DUR_EIGHTH),N(NOTE_E5, DUR_EIGHTH),
    N(NOTE_D5, DUR_EIGHTH),N(NOTE_E5, DUR_QTR),
};

// Nyx OS Theme (original!)
// This melody was already built around big sustained notes, which
// makes it the best showcase for the chord engine — every long tone
// becomes a real major triad instead of a single flat pitch.
static const Note nyx_theme[] = {
    N(NOTE_C5, DUR_QTR),  N(NOTE_E5, DUR_QTR),
    N(NOTE_G5, DUR_QTR),  CHORD(NOTE_C6, NOTE_G5, NOTE_E5, DUR_HALF),
    REST(DUR_EIGHTH),
    N(NOTE_B4, DUR_QTR),  N(NOTE_D5, DUR_QTR),
    N(NOTE_G5, DUR_QTR),  CHORD(NOTE_B5, NOTE_G5, NOTE_D5, DUR_HALF),
    REST(DUR_EIGHTH),
    N(NOTE_A4, DUR_QTR),  N(NOTE_C5, DUR_QTR),
    N(NOTE_E5, DUR_QTR),  CHORD(NOTE_A5, NOTE_E5, NOTE_C5, DUR_HALF),
    REST(DUR_EIGHTH),
    N(NOTE_G4, DUR_QTR),  N(NOTE_B4, DUR_QTR),
    N(NOTE_D5, DUR_QTR),  CHORD(NOTE_G5, NOTE_D5, NOTE_B4, DUR_WHOLE),
    REST(DUR_QTR),
    N(NOTE_C5, DUR_EIGHTH),N(NOTE_D5, DUR_EIGHTH),
    N(NOTE_E5, DUR_QTR),  N(NOTE_G5, DUR_QTR),
    N(NOTE_E5, DUR_QTR),  CHORD(NOTE_C5, NOTE_E5, NOTE_G4, DUR_HALF),
};

// ─── Playlist ─────────────────────────────────────────────────────
const Song playlist[] = {
    { "Nyx OS Theme",       "Nyx OS",      nyx_theme,      sizeof(nyx_theme)      / sizeof(Note) },
    { "Twinkle Twinkle",    "Traditional", twinkle,        sizeof(twinkle)        / sizeof(Note) },
    { "Ode to Joy",         "Beethoven",   ode_to_joy,     sizeof(ode_to_joy)     / sizeof(Note) },
    { "Happy Birthday",     "Traditional", happy_birthday, sizeof(happy_birthday) / sizeof(Note) },
    { "Imperial March",     "John Williams",imperial_march, sizeof(imperial_march) / sizeof(Note) },
};
const int playlist_count = 5;

// ─── Player State ─────────────────────────────────────────────────
static int      current_song    = 0;
static uint32_t current_note    = 0;
static uint32_t note_ticks      = 0;
static int      is_playing      = 0;
static int      is_paused       = 0;
static uint32_t chord_phase     = 0;   /* drives arp/vibrato animation */

// ─── Music Tick (called by timer IRQ) ────────────────────────────
void music_tick() {
    if (!is_playing || is_paused) return;

    const Song* song = &playlist[current_song];

    if (note_ticks > 0) {
        note_ticks--;
        // Small gap between notes (last 2 ticks silent) — but only
        // for plain notes; chord/arp/vibrato notes ring through to
        // the end since the alternation itself gives a sense of
        // phrasing, and cutting them early sounds abrupt.
        const Note* cur = &song->notes[current_note > 0 ? current_note - 1 : 0];
        if (note_ticks == 2 && cur->style == PLAY_PLAIN) {
            speaker_stop();
            return;
        }
        // Re-drive the oscillator every tick for the duration of the
        // note so chord/arp/vibrato styles keep animating instead of
        // freezing on whichever tone happened to be set when the
        // note started.
        if (current_note > 0) {
            const Note* n = &song->notes[current_note - 1];
            chord_phase++;
            if (n->frequency == NOTE_REST) {
                speaker_stop();
            } else if (n->style != PLAY_PLAIN || n->harmony1 || n->harmony2) {
                speaker_chord_tick(n->frequency, n->harmony1, n->harmony2,
                                   n->style, chord_phase);
            }
            // PLAY_PLAIN with no harmony: leave the tone speaker_play()
            // already set when the note started — no need to re-set
            // the same divisor every tick, keeps old behavior identical.
        }
        return;
    }

    // Move to next note
    if (current_note >= song->length) {
        // Song finished — stop
        current_note = 0;
        chord_phase  = 0;
        is_playing   = 0;
        speaker_stop();
        return;
    }

    // Play current note
    const Note* n = &song->notes[current_note];
    chord_phase = 0;
    if (n->frequency == NOTE_REST)
        speaker_stop();
    else if (n->style != PLAY_PLAIN || n->harmony1 || n->harmony2)
        speaker_chord_tick(n->frequency, n->harmony1, n->harmony2, n->style, 0);
    else
        speaker_play(n->frequency);

    note_ticks = n->duration;
    current_note++;
}

// ─── Controls ─────────────────────────────────────────────────────
void music_play(int song_index) {
    current_song = song_index % playlist_count;
    current_note = 0;
    chord_phase  = 0;
    note_ticks   = 0;
    is_playing   = 1;
    is_paused    = 0;
}

void music_stop() {
    is_playing   = 0;
    is_paused    = 0;
    current_note = 0;
    chord_phase  = 0;
    note_ticks   = 0;
    speaker_stop();
}

void music_pause() {
    if (!is_playing) return;
    is_paused = !is_paused;
    if (is_paused)
        speaker_stop();
}

void music_next() {
    current_song = (current_song + 1) % playlist_count;
    current_note = 0;
    chord_phase  = 0;
    note_ticks   = 0;
    is_playing   = 1;
    is_paused    = 0;
}

void music_prev() {
    current_song = (current_song + playlist_count - 1) % playlist_count;
    current_note = 0;
    chord_phase  = 0;
    note_ticks   = 0;
    is_playing   = 1;
    is_paused    = 0;
}

int music_is_playing()  { return is_playing && !is_paused; }
int music_current_song(){ return current_song; }
int music_get_note_progress(){
    if (!is_playing || is_paused || current_note == 0) return 0;
    const Song* song = &playlist[current_song];
    if (current_note > song->length) return 0;
    const Note* n = &song->notes[current_note - 1];
    if (n->duration == 0) return 0;
    return (n->duration - note_ticks) * 255 / n->duration;
}
int music_current_style(){
    if (!is_playing || is_paused || current_note == 0) return PLAY_PLAIN;
    const Song* song = &playlist[current_song];
    if (current_note > song->length) return PLAY_PLAIN;
    return (int)song->notes[current_note - 1].style;
}
