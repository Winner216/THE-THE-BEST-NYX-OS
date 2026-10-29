// include/gifdec.h — Minimal GIF decoder for Nyx OS
#ifndef GIFDEC_H
#define GIFDEC_H

#include <stdint.h>

// Decoded image info
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t* pixels;   // 32-bit RGBA (first frame only)
} GifImage;

// Decode the first frame of a GIF87a/89a from raw file data.
// Returns 1 on success, 0 on failure.
// The caller must provide a pixel buffer of at least width*height*4 bytes.
int gif_decode(const uint8_t* data, int datalen, GifImage* img);

#endif
