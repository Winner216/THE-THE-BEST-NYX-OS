// include/bmpdec.h — Minimal BMP decoder for Nyx OS
#ifndef BMPDEC_H
#define BMPDEC_H

#include <stdint.h>

// Decoded image info
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t* pixels;   // 32-bit RGBA
} BmpImage;

// Decode a BMP from raw file data (uncompressed 24/32-bit only).
// Returns 1 on success, 0 on failure.
// The caller must provide a pixel buffer of at least width*height*4 bytes.
int bmp_decode(const uint8_t* data, int datalen, BmpImage* img);

#endif
