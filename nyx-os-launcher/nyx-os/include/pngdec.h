// include/pngdec.h — Minimal PNG decoder for Nyx OS
#ifndef PNGDEC_H
#define PNGDEC_H

#include <stdint.h>

// Decoded image info
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t* pixels;   // 32-bit RGBA (premultiplied alpha)
} PngImage;

// Decode a PNG from raw file data.
// Returns 1 on success, 0 on failure.
// The caller must provide a pixel buffer of at least width*height*4 bytes.
int png_decode(const uint8_t* data, int datalen, PngImage* img);

// Inflate (raw DEFLATE decompression, RFC 1951) — exported so other
// subsystems can reuse the decoder, e.g. http.c for gzip bodies.
// Reads compressed data from 'in', writes uncompressed data to 'out'.
// Returns the number of bytes written, or -1 on error. The internal
// 32 KB LZ77 window is static (BSS), so this is safe to call from
// deep call stacks without growing the kernel stack.
int png_inflate(const uint8_t* in, int in_len, uint8_t* out, int out_max);

#endif
