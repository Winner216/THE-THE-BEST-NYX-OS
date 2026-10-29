// include/svgdec.h — Minimal SVG rasterizer for Nyx OS
#ifndef SVGDEC_H
#define SVGDEC_H

#include <stdint.h>

// Decoded image info
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t* pixels;   // 32-bit RGBA
} SvgImage;

// Decode a minimal SVG subset from raw text data (see cpu/svgdec.c
// for the supported subset).  Returns 1 on success, 0 on failure.
// The caller must provide a pixel buffer of at least width*height*4
// bytes — call svg_dims() first to size it.
int svg_decode(const uint8_t* data, int datalen, SvgImage* img);

// Parse only the <svg> header (width/height/viewBox) to get the
// rendered dimensions without rasterizing.  Returns 1 on success.
int svg_dims(const uint8_t* data, int datalen, int* w, int* h);

#endif
