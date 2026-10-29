// cpu/bmpdec.c — Minimal BMP decoder for Nyx OS
//
// Supported subset:
//   - 24-bit uncompressed (BI_RGB) — bottom-up or top-down
//   - 32-bit uncompressed (BI_RGB) — alpha channel preserved
//   - OS/2 BITMAPCOREHEADER (12-byte header) + Windows BITMAPINFOHEADER (40-byte)
//
// Limitations:
//   - No RLE compression (BI_RLE8, BI_RLE4)
//   - No bitfields (BI_BITFIELDS)
//   - No 1/4/8-bit palette images
//   - No 16-bit images

#include <bmpdec.h>
#include <stdint.h>
#include <stddef.h>

int bmp_decode(const uint8_t* data, int datalen, BmpImage* img) {
    if (!data || !img || datalen < 26) return 0;

    // Check magic "BM"
    if (data[0] != 'B' || data[1] != 'M') return 0;

    // Pixel data offset at bytes 10-13 (little-endian)
    uint32_t pix_offset = (uint32_t)data[10]
                        | ((uint32_t)data[11] << 8)
                        | ((uint32_t)data[12] << 16)
                        | ((uint32_t)data[13] << 24);

    // DIB header size at bytes 14-17
    uint32_t header_sz = (uint32_t)data[14]
                       | ((uint32_t)data[15] << 8)
                       | ((uint32_t)data[16] << 16)
                       | ((uint32_t)data[17] << 24);

    int width = 0, height = 0, bpp = 24;

    if (header_sz == 12) {
        // BITMAPCOREHEADER (OS/2 v1)
        if (datalen < 26) return 0;
        width  = (int)data[18] | ((int)data[19] << 8);
        height = (int)data[20] | ((int)data[21] << 8);
        bpp    = (int)data[24] | ((int)data[25] << 8);
    } else if (header_sz >= 40) {
        // BITMAPINFOHEADER or newer
        if (datalen < 54) return 0;
        width  = (int)(data[18] | ((int)data[19] << 8) | ((int)data[20] << 16) | ((int)data[21] << 24));
        height = (int)(data[22] | ((int)data[23] << 8) | ((int)data[24] << 16) | ((int)data[25] << 24));
        bpp    = (int)data[28] | ((int)data[29] << 8);
    } else {
        return 0; // Unknown header format
    }

    // Validate BOTH dimensions before any pixel write — height was
    // previously uncapped (only width was), so a crafted header could
    // drive abs_h up to 2^31 rows and overflow the caller's buffer.
    if (width <= 0 || height == 0 || width > 4096 ||
        height > 4096 || height < -4096) return 0;
    if (bpp != 24 && bpp != 32) return 0;
    if ((uint32_t)pix_offset >= (uint32_t)datalen) return 0;

    int abs_h = height < 0 ? -height : height;
    int bottom_up = (height > 0);  // positive height = bottom-up

    // BMP rows are padded to 4-byte alignment
    int row_bytes = width * (bpp / 8);
    int row_pad = (4 - (row_bytes & 3)) & 3;
    int stride = row_bytes + row_pad;

    img->width = (uint32_t)width;
    img->height = (uint32_t)abs_h;

    for (int y = 0; y < abs_h; y++) {
        int src_y = bottom_up ? (abs_h - 1 - y) : y;
        int src_off = pix_offset + src_y * stride;

        if (src_off + row_bytes > datalen) return 0;

        uint8_t* dst = img->pixels + y * width * 4;

        for (int x = 0; x < width; x++) {
            int si = src_off + x * (bpp / 8);
            dst[x * 4 + 0] = data[si + 2];  // R
            dst[x * 4 + 1] = data[si + 1];  // G
            dst[x * 4 + 2] = data[si + 0];  // B
            dst[x * 4 + 3] = (bpp == 32) ? data[si + 3] : 255;  // A
        }
    }

    return 1;
}
