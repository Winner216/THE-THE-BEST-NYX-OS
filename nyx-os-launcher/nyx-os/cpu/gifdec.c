// cpu/gifdec.c — Minimal GIF decoder for Nyx OS
//
// Supported subset:
//   - GIF87a and GIF89a
//   - Any color resolution (up to 8 bits)
//   - Interlaced and non-interlaced
//   - Global and local color tables
//
// Limitations:
//   - Decodes the FIRST frame only
//   - No animation / frame delay handling
//   - No transparent color disposal
//   - No GIF extension processing beyond graphics control

#include <gifdec.h>
#include <stdint.h>
#include <stddef.h>

// ─── LZW decompression ───────────────────────────────────────────
// GIF LZW: variable-width codes, starting at CODE_SIZE+1 bits,
// growing up to 12 bits.  Clear code = 1 << code_size,
// End-of-info code = clear_code + 1.

#define LZW_MAX_CODES 4096
#define LZW_STACK_SIZE 4096

static int gif_lzw_decompress(const uint8_t* in, int in_len,
                               uint8_t* out, int out_len,
                               int min_code_size) {
    // Temporary storage for LZW
    static int   prefix[LZW_MAX_CODES];
    static int   suffix[LZW_MAX_CODES];
    static uint8_t stack[LZW_STACK_SIZE];

    if (min_code_size < 2) min_code_size = 2;
    if (min_code_size > 8) return 0;

    int clear = 1 << min_code_size;
    int eoi   = clear + 1;
    int avail = clear + 2;
    int code_size = min_code_size + 1;
    int max_code = (1 << code_size);

    // Initialize single-symbol entries
    for (int i = 0; i < clear; i++) {
        prefix[i] = -1;
        suffix[i] = (uint8_t)i;
    }

    int bits = 0;
    int bits_left = 0;
    int byte_pos = 0;
    int out_pos = 0;
    int old_code = -1;
    int first_byte = 0;

    // Sub-block reading: GIF data is in sub-blocks (length-byte + data)
    // For simplicity, assume 'in' is the raw sub-block data concatenated
    // without the length bytes (as stored in the image data section)

    while (byte_pos < in_len && out_pos < out_len) {
        // Refill bit buffer
        while (bits_left < code_size && byte_pos < in_len) {
            bits |= (in[byte_pos++] << bits_left);
            bits_left += 8;
        }
        if (bits_left < code_size && byte_pos >= in_len) break;

        int code = bits & ((1 << code_size) - 1);
        bits >>= code_size;
        bits_left -= code_size;

        if (code == eoi) break;

        if (code == clear) {
            // Reset
            avail = clear + 2;
            code_size = min_code_size + 1;
            max_code = (1 << code_size);
            old_code = -1;
            continue;
        }

        if (old_code < 0) {
            // First code after reset
            if (out_pos < out_len) {
                out[out_pos++] = (uint8_t)(code < LZW_MAX_CODES ? suffix[code] : 0);
                old_code = code;
                first_byte = (code < LZW_MAX_CODES) ? suffix[code] : 0;
            }
            continue;
        }

        // Decompress code to stack
        int sp = 0;
        int current = code;

        if (code >= avail) {
            // Special case: code not yet in table (use old_code + first byte)
            if (sp < LZW_STACK_SIZE) {
                stack[sp++] = (uint8_t)first_byte;
            }
            current = old_code;
        }

        while (current >= 0 && current < LZW_MAX_CODES) {
            if (sp >= LZW_STACK_SIZE) break;
            stack[sp++] = suffix[current];
            current = prefix[current];
            if (current < -1 || current >= LZW_MAX_CODES) break;
        }

        if (sp <= 0) continue;

        // Pop stack to output
        for (int i = sp - 1; i >= 0 && out_pos < out_len; i--) {
            out[out_pos++] = stack[i];
        }
        first_byte = stack[sp - 1];

        // Add new entry to dictionary
        if (avail < LZW_MAX_CODES) {
            prefix[avail] = old_code;
            suffix[avail] = first_byte;
            avail++;
        }

        if (avail > max_code && code_size < 12) {
            code_size++;
            max_code = (1 << code_size);
        }

        old_code = code;
    }

    return out_pos;
}

// ─── Read a 2-byte little-endian unsigned int ────────────────────
static int read_u16le(const uint8_t* p) {
    return (int)p[0] | ((int)p[1] << 8);
}

// ─── GIF decode ──────────────────────────────────────────────────
int gif_decode(const uint8_t* data, int datalen, GifImage* img) {
    if (!data || !img || datalen < 14) return 0;

    // Check GIF signature
    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F') return 0;
    if (data[3] != '8' || (data[4] != '7' && data[4] != '9') || data[5] != 'a') return 0;

    // Logical screen descriptor
    int width  = read_u16le(data + 6);
    int height = read_u16le(data + 8);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return 0;

    uint8_t packed = data[10];
    int gct_flag = (packed >> 7) & 1;
    int gct_size = 1 << ((packed & 0x07) + 1);  // bits 0-2

    // Background color index
    // int bg_color = data[11];

    // Pixel aspect ratio — skip
    int pos = 13;

    // Read global color table if present
    uint8_t global_pal[256][3];
    int global_pal_entries = 0;
    if (gct_flag) {
        global_pal_entries = gct_size;
        if (pos + gct_size * 3 > datalen) return 0;
        for (int i = 0; i < gct_size; i++) {
            global_pal[i][0] = data[pos + i * 3 + 0];
            global_pal[i][1] = data[pos + i * 3 + 1];
            global_pal[i][2] = data[pos + i * 3 + 2];
        }
        pos += gct_size * 3;
    }

    // Default palette for images without local table
    int pal_entries = global_pal_entries;
    uint8_t (*palette)[3] = global_pal;

    // Scan blocks until we find an image descriptor (0x2C)
    int img_x = 0, img_y = 0, img_w = 0, img_h = 0;
    int interlace = 0;
    uint8_t lzw_min_code = 0;
    // MUST be static (not stack)!  The kernel stack is only 96 KB
    // (boot/boot.asm) and LZW data for a full-screen GIF can be tens
    // of KB.  A stack array here would overflow into the memory below
    // the stack and silently corrupt it.
    static uint8_t lzw_data[65536];
    int lzw_len = 0;

    while (pos < datalen) {
        uint8_t block_type = data[pos++];

        if (block_type == 0x2C) {
            // Image descriptor
            if (pos + 8 > datalen) return 0;
            img_x = read_u16le(data + pos);
            img_y = read_u16le(data + pos + 2);
            img_w = read_u16le(data + pos + 4);
            img_h = read_u16le(data + pos + 6);
            if (img_w <= 0 || img_h <= 0) return 0;
            pos += 8;

            // Image descriptor packed field
            if (pos >= datalen) return 0;
            uint8_t img_packed = data[pos++];
            interlace = (img_packed >> 6) & 1;
            int lct_flag = (img_packed >> 7) & 1;
            int lct_size = 1 << ((img_packed & 0x07) + 1);

            // Local color table overrides global
            if (lct_flag) {
                pal_entries = lct_size;
                for (int i = 0; i < lct_size && pos < datalen; i++) {
                    palette[i][0] = data[pos++];
                    palette[i][1] = data[pos++];
                    palette[i][2] = data[pos++];
                }
            }

            // LZW minimum code size
            if (pos >= datalen) return 0;
            lzw_min_code = data[pos++];

            // Read sub-blocks (LZW compressed data)
            lzw_len = 0;
            while (pos < datalen) {
                int sb_len = data[pos++];
                if (sb_len == 0) break;  // block terminator
                for (int i = 0; i < sb_len && pos < datalen && lzw_len < 65536; i++) {
                    lzw_data[lzw_len++] = data[pos++];
                }
            }

            // We found the first image — stop scanning
            break;

        } else if (block_type == 0x21) {
            // Extension block
            if (pos >= datalen) return 0;
            uint8_t ext_label = data[pos++];
            // Skip extension sub-blocks
            while (pos < datalen) {
                int sb_len = data[pos++];
                if (sb_len == 0) break;
                pos += sb_len;
                if (pos > datalen) return 0;
            }
            (void)ext_label;  // We don't process extensions

        } else if (block_type == 0x3B) {
            // Trailer — no image found
            break;

        } else {
            // Unknown block — skip sub-blocks
            while (pos < datalen) {
                int sb_len = data[pos++];
                if (sb_len == 0) break;
                pos += sb_len;
                if (pos > datalen) return 0;
            }
        }
    }

    if (img_w <= 0 || img_h <= 0 || lzw_len == 0) return 0;

    // Clip dimensions to logical screen (should match, but be safe)
    if (img_x + img_w > width)  img_w = width - img_x;
    if (img_y + img_h > height) img_h = height - img_y;
    if (img_w <= 0 || img_h <= 0) return 0;

    // Decompress LZW data
    int raw_stride = width;  // one byte per pixel (palette index)
    int raw_size = raw_stride * img_h;
    // MUST be static (not stack): a 256 KB array on a 96 KB kernel
    // stack would overflow by ~160 KB before decompression even
    // starts.  Same reasoning as lzw_data above.
    static uint8_t raw[262144];
    if (raw_size > (int)sizeof(raw)) return 0;

    int decoded = gif_lzw_decompress(lzw_data, lzw_len, raw, raw_size, lzw_min_code);
    if (decoded <= 0) return 0;

    img->width = (uint32_t)width;
    img->height = (uint32_t)height;

    // Fill output pixels: start with background color, then overlay image
    for (int i = 0; i < width * height; i++) {
        img->pixels[i * 4 + 0] = 0;
        img->pixels[i * 4 + 1] = 0;
        img->pixels[i * 4 + 2] = 0;
        img->pixels[i * 4 + 3] = 255;
    }

    // Deinterlace helper
    int interlace_start[] = {0, 4, 2, 1};
    int interlace_step[]  = {8, 8, 4, 2};

    for (int y = 0; y < img_h; y++) {
        int src_line = y;
        if (interlace) {
            // Map scanline order to interlace pass
            int written = 0;
            for (int pass = 0; pass < 4; pass++) {
                for (int line = interlace_start[pass]; line < img_h; line += interlace_step[pass]) {
                    if (written == y) { src_line = line; break; }
                    written++;
                }
                if (written > y) break;
            }
        }

        for (int x = 0; x < img_w; x++) {
            int idx = raw[src_line * raw_stride + x];
            if (idx >= pal_entries) idx = 0;

            int dst_y = img_y + y;
            int dst_x = img_x + x;
            if (dst_y >= height || dst_x >= width) continue;

            int pi = (dst_y * width + dst_x) * 4;
            img->pixels[pi + 0] = palette[idx][0];
            img->pixels[pi + 1] = palette[idx][1];
            img->pixels[pi + 2] = palette[idx][2];
            img->pixels[pi + 3] = 255;
        }
    }

    return 1;
}
