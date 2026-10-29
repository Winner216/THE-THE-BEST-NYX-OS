#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>

// ─── VESA video player ───────────────────────────────────────────
// Plays MP4 container files whose video track is MJPEG (the only
// codec the kernel can decode).  Opens from the Files app via
// files_open_file(); ESC returns to the caller (see
// video_return_mode in desktop.c).
//
// video_init()   — parse container, build the MJPEG frame index
// video_draw()   — render the player window (title, video, transport)
// video_handle_key()  — space = play/pause, arrows = seek
// video_handle_mouse()— play button / seek bar
// video_tick()   — advance playback on the PIT heartbeat clock;
//                  returns 1 when a frame advanced (redraw needed).
//                  While a PCM sidecar is playing, the audio clock
//                  is the MASTER timeline: the playhead follows the
//                  sound position, and non-reference MJPEG frames
//                  (all of them — each is independently decodable)
//                  are dropped when decode falls behind, keeping the
//                  picture in step with the audio.  When the picture
//                  runs ahead of the sound it briefly holds (repeats
//                  the last frame) instead of jumping, re-anchoring
//                  only if the lead grows too large to wait out.  A
//                  "DROP n" indicator in the status line counts the
//                  skips.
void video_init(const uint8_t* data, int len, const char* name);
// Attach a <name>.pcm audio sidecar (8-bit unsigned mono PCM at
// AUDIO_PCM_RATE) so playback has sound.  Call after video_init().
void video_set_audio(const uint8_t* pcm, int len, int rate);
// Prefer the MP3 track muxed inside the MP4 (tools/transcode-videos.py
// puts it there): decoded in the background at 44.1 kHz stereo.  Falls
// back to the .pcm sidecar when the container has no audio track.
void video_set_audio_mp3(void);
// 1 if the container carries a muxed MP3 audio track.
int  video_has_mp3(void);
// Close the player (ESC or the [X] button) — stops audio immediately.
void video_close(void);
void video_draw(void);
void video_handle_key(char c);
int  video_handle_mouse(int clk_x, int clk_y);
int  video_tick(void);
// Decode-ahead: pre-decode the next frame into the player's back
// buffer during the current frame's window.  Call from
// desktop_video_pump (main-loop context) right after the frame
// redraw, so the next present at PTS is a pure blit.
void video_pump_ahead(void);

#endif
