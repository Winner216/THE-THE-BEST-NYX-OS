// include/music.h
#ifndef MUSIC_H
#define MUSIC_H

#include <stdint.h>

// Note frequencies (Hz)
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_B5  988
#define NOTE_C6  1047
#define NOTE_REST 0

// Note durations (in timer ticks)
#define DUR_WHOLE  72
#define DUR_HALF   36
#define DUR_QTR    18
#define DUR_EIGHTH  9
#define DUR_SIXTEENTH 4

// Playback style for a note — controls how the single PC-speaker
// oscillator is driven to fake richer sound than one flat tone.
typedef enum {
    PLAY_PLAIN   = 0,   /* one steady tone (original behavior)        */
    PLAY_CHORD   = 1,   /* fast-alternate root/harmony1/harmony2       */
    PLAY_ARP     = 2,   /* slower up-down arpeggio across the chord    */
    PLAY_VIBRATO = 3,   /* tiny frequency wobble around the root tone  */
} PlayStyle;

typedef struct {
    uint32_t  frequency;   /* root tone, 0 = rest                      */
    uint32_t  duration;    /* length in 18.2Hz timer ticks             */
    uint32_t  harmony1;    /* 0 = none. A third/fifth above the root   */
    uint32_t  harmony2;    /* 0 = none. A second harmony tone          */
    PlayStyle style;       /* how to render the tone(s) above          */
} Note;

// ─── Note-construction shorthand ───────────────────────────────────
// Keeps song tables readable: a single melody tone is just
// N(NOTE_E4, DUR_QTR); a three-tone chord that fakes simultaneous
// harmony is CHORD(root, third, fifth, duration); a rolling arpeggio
// across the same three tones is ARP(...); and VIB(...) adds a tiny
// pitch wobble to a single sustained tone so it doesn't sound dead.
#define N(freq, dur)                  { (freq), (dur), 0, 0, PLAY_PLAIN }
#define CHORD(root, h1, h2, dur)      { (root), (dur), (h1), (h2), PLAY_CHORD }
#define CHORD2(root, h1, dur)         { (root), (dur), (h1), 0,   PLAY_CHORD }
#define ARP(root, h1, h2, dur)        { (root), (dur), (h1), (h2), PLAY_ARP }
#define VIB(freq, dur)                { (freq), (dur), 0, 0, PLAY_VIBRATO }
#define REST(dur)                     { NOTE_REST, (dur), 0, 0, PLAY_PLAIN }

typedef struct {
    const char* title;
    const char* artist;
    const Note* notes;
    uint32_t    length;
} Song;

void music_init(void);
void music_tick(void);
void music_play(int song_index);
void music_stop(void);
void music_pause(void);
void music_next(void);
void music_prev(void);
int  music_is_playing(void);
int  music_current_song(void);
/* Returns the PlayStyle of whatever note is currently sounding
   (0=plain, 1=chord, 2=arp, 3=vibrato), or PLAY_PLAIN if nothing is
   playing. Lets the UI show a live "harmony active" indicator. */
int  music_current_style(void);
/* Returns 0–255 indicating how far through the current note has
   progressed (0=just started, 255=about to end). Useful for syncing
   UI animations (pulse/glow) to the music's beat. Returns 0 if
   nothing is playing or between notes. */
int  music_get_note_progress(void);

extern const Song playlist[];
extern const int  playlist_count;

#endif
