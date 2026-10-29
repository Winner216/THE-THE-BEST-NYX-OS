// include/mp3dec.h — MPEG-1 Layer III (MP3) decoder for Nyx OS
//
// Pure-integer, freestanding decoder implementing the ISO/IEC 11172-3
// Layer III pipeline:
//   1. Bitstream parser — frame sync (0xFFE), header (bitrate, sample
//      rate, channels), side information, bit-reservoir main data.
//   2. Huffman decoding — the 34 ISO tables (generated as binary tries
//      in cpu/mp3tables.c) for big-values and count1 regions.
//   3. Quantization & reordering — x^(4/3) dequantization with
//      scalefactors/global_gain, and short-block reordering.
//   4. Stereo processing — mid/side and intensity stereo.
// plus the IMDCT (long/short/mixed blocks) and the 32-band polyphase
// synthesis filterbank that produce the final PCM.
//
// Everything is integer fixed-point (no FPU on this kernel), and the
// numeric scale matches the well-tested reference float decoder, so the
// int16 output needs no extra normalisation shift.
//
// The decoder is MPEG1-only by design: files with the MPEG2/2.5 version
// bits are rejected cleanly (returns 0 from mp3_decode_frame).

#ifndef MP3DEC_H
#define MP3DEC_H

#include <stdint.h>
#include <mp3tables.h>

#define MP3_MAX_FRAME_BYTES  2881   // 320 kbps @ 32 kHz + padding
#define MP3_RESERV_BYTES     511    // max main_data_begin
#define MP3_FRAME_SAMPLES    1152   // MPEG1 Layer III samples per frame

typedef struct {
    const uint8_t* data;
    int  size;
    int  frame_off;          // byte offset of the current frame header
    int  frame_bytes;        // size of the current frame (incl. header)
    // header fields
    int  channels;           // 1 or 2
    int  samplerate;         // Hz
    int  bitrate;            // kbps
    int  sr_idx;             // 5..7 (row into the sfb tables)
    int  ms_stereo;          // 1 = mid/side mode
    int  i_stereo;           // 1 = intensity stereo mode
    // last valid header (for cross-frame sync)
    uint8_t last_hdr[4];
    // bit reservoir
    uint8_t reserv[MP3_RESERV_BYTES];
    int  reserv_len;
    // per-frame main data assembly buffer
    uint8_t maindata[MP3_RESERV_BYTES + MP3_MAX_FRAME_BYTES];
    // scalefactor history per channel (for scfsi reuse across granules)
    uint8_t iscf_hist[2][40];
    // intensity-stereo position history (channel 1 only)
    uint8_t ist_hist[40];
    // DSP state
    int32_t overlap[2][288];       // 2ch x 32 subbands x 9 overlap
    int32_t qmf[2][960];           // synthesis filter state per channel
    int32_t syn[2112 + 64];        // linear-interp working buffer (lins)
    int32_t grbuf[1152];           // 2ch x 576 spectral / time samples
    int32_t reorder_scratch[576];
    int16_t pcm_flat[2 * MP3_FRAME_SAMPLES];  // interleaved synth output
    // output
    int16_t pcm[2][MP3_FRAME_SAMPLES];   // [channel][sample]
    int  pcm_samples;              // samples written per channel this frame
} Mp3Dec;

// Initialise / rewind a decoder context around a whole MP3 file.  Returns
// the byte offset of the first MPEG frame (skipping an ID3v2 tag), or -1
// if the file has no valid MPEG1 Layer III frame.  Sets samplerate,
// channels and bitrate from that first frame.
int  mp3_init(Mp3Dec* d, const uint8_t* data, int size);

// Parse just the header at data[off] (no state change).  Returns frame
// bytes (incl. header) or 0 if it isn't a valid MPEG1 Layer III header.
int  mp3_header_frame_bytes(const uint8_t* data, int size, int off);

// Decode the frame at d->data + d->frame_off.  On success fills
// d->pcm[ch][0..1152), sets d->pcm_samples = 1152, advances frame_off
// to the next frame and returns 1152.  Returns 0 at end-of-stream or on
// a corrupt/unsupported frame (stream resync attempted).
int  mp3_decode_frame(Mp3Dec* d);

#endif
