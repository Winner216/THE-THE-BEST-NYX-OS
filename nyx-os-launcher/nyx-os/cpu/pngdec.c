// cpu/pngdec.c — Minimal PNG decoder for Nyx OS
//
// Supported subset:
//   - 8-bit depth only
//   - Color type 2 (RGB), 3 (indexed/palette), and 6 (RGBA)
//   - Non-interlaced only
//   - All DEFLATE block types (stored, fixed Huffman, dynamic Huffman)
//
// Limitations:
//   - Skips CRC validation (saves ~1KB of table data)
//   - No grayscale (color types 0, 4)
//   - No Adam7 interlacing

#include <pngdec.h>
#include <stdint.h>
#include <stddef.h>

// ─── Bit reader (LSB-first for DEFLATE) ──────────────────────────
typedef struct {
    const uint8_t* data;
    int            len;
    int            pos;   // byte position
    int            bits;  // bits remaining in current byte (filled from MSB)
    unsigned int   buf;   // current byte buffer (MUST be unsigned — right shifts on signed ints are arithmetic and corrupt the stream!)
} BitReader;

static void br_init(BitReader* br, const uint8_t* d, int len) {
    br->data = d; br->len = len; br->pos = 0; br->bits = 0; br->buf = 0;
}

static int br_fill(BitReader* br) {
    while (br->bits <= 24 && br->pos < br->len) {
        br->buf |= (br->data[br->pos] << br->bits);
        br->bits += 8;
        br->pos++;
    }
    return br->bits > 0 ? 1 : 0;
}

static int br_peek_bits(BitReader* br, int n) {
    if (!br_fill(br) && br->bits < n) return -1;
    return br->buf & ((1 << n) - 1);
}

static void br_skip_bits(BitReader* br, int n) {
    br->buf >>= n;
    br->bits -= n;
}

static int br_read_bit(BitReader* br) {
    int v = br_peek_bits(br, 1);
    if (v >= 0) br_skip_bits(br, 1);
    return v;
}

static int br_read_bits(BitReader* br, int n) {
    int v = br_peek_bits(br, n);
    if (v >= 0) br_skip_bits(br, n);
    return v;
}

/* br_read_bits_msb — read n bits MSB-first, unused here, kept for JPEG decoder */

// ─── Huffman tree (tree-based, memory efficient) ─────────────────
#define HT_MAX_NODES 600

typedef struct {
    int16_t kids[2];   // child indices, -1 = none
    int16_t sym;       // symbol if leaf, -1 if internal
} HtNode;

typedef struct {
    HtNode nodes[HT_MAX_NODES];
    int    count;
} HtTree;

static void ht_init(HtTree* t) {
    t->count = 0;
}

static int ht_new_node(HtTree* t) {
    if (t->count >= HT_MAX_NODES) return -1;
    int idx = t->count++;
    t->nodes[idx].kids[0] = -1;
    t->nodes[idx].kids[1] = -1;
    t->nodes[idx].sym = -1;
    return idx;
}

// Build a Huffman tree from canonical code lengths.
// lengths[i] = bit length for symbol i (0 = unused)
// num_syms = number of symbols
static int ht_build(HtTree* t, const int* lengths, int num_syms) {
    ht_init(t);
    int root = ht_new_node(t);
    if (root < 0) return -1;

    // Validate: find max length
    int max_len = 0;
    for (int i = 0; i < num_syms; i++)
        if (lengths[i] > max_len) max_len = lengths[i];
    if (max_len > 15) return -1;

    // Canonical Huffman: assign codes in order
    // Count codes of each length
    int bl_count[16] = {0};
    for (int i = 0; i < num_syms; i++)
        if (lengths[i] > 0) bl_count[lengths[i]]++;

    // Compute starting code for each length
    int next_code[16] = {0};
    int code = 0;
    for (int bits = 1; bits <= max_len; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // Insert each symbol into the tree
    for (int sym = 0; sym < num_syms; sym++) {
        int len = lengths[sym];
        if (len == 0) continue;

        int c = next_code[len]++;
        int node = root;

        // Walk tree MSB first (canonical codes are MSB-first)
        for (int b = len - 1; b >= 0; b--) {
            int bit = (c >> b) & 1;
            if (t->nodes[node].kids[bit] < 0) {
                int n = ht_new_node(t);
                if (n < 0) return -1;
                t->nodes[node].kids[bit] = (int16_t)n;
            }
            node = t->nodes[node].kids[bit];
        }
        t->nodes[node].sym = (int16_t)sym;
    }
    return root;
}

// Decode one symbol from the bit stream using the tree.
static int ht_decode(HtTree* t, int root, BitReader* br) {
    int node = root;
    while (t->nodes[node].sym < 0) {
        int bit = br_read_bit(br);
        if (bit < 0) return -1;
        int kid = t->nodes[node].kids[bit];
        if (kid < 0) return -1;  // invalid code
        node = kid;
    }
    return t->nodes[node].sym;
}

// ─── DEFLATE fixed Huffman tables ────────────────────────────────
static int deflate_lit_len[288];  // 0-285
static int deflate_dist[32];      // 0-29
static int deflate_tables_inited = 0;

static void deflate_init_tables(void) {
    if (deflate_tables_inited) return;
    deflate_tables_inited = 1;

    // Literal/length: 0-143 → 8 bits, 144-255 → 9 bits, 256-279 → 7 bits, 280-287 → 8 bits
    // (Actually per RFC 1951: 0-143 length 8, 144-255 length 9, 256-279 length 7, 280-287 length 8)
    for (int i = 0; i <= 143; i++) deflate_lit_len[i] = 8;
    for (int i = 144; i <= 255; i++) deflate_lit_len[i] = 9;
    for (int i = 256; i <= 279; i++) deflate_lit_len[i] = 7;
    for (int i = 280; i <= 287; i++) deflate_lit_len[i] = 8;

    // Distance: 0-29 → 5 bits (fixed)
    for (int i = 0; i < 30; i++) deflate_dist[i] = 5;
    for (int i = 30; i < 32; i++) deflate_dist[i] = 0;
}

// Length extra bits table
static const int len_base[] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const int len_extra[] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};

// Distance extra bits table
static const int dist_base[] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const int dist_extra[] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

// ─── Inflate (DEFLATE decompression) ─────────────────────────────
// Reads compressed data from 'in', writes uncompressed data to 'out'.
// Returns number of bytes written, or -1 on error. Exported as
// png_inflate (see pngdec.h) so http.c can decode gzip bodies.
int png_inflate(const uint8_t* in, int in_len, uint8_t* out, int out_max) {
    BitReader br;
    br_init(&br, in, in_len);

    HtTree lt, dt, clt;
    int out_pos = 0;
    int bfinal = 0;

    // Sliding window for LZ77 backreferences.
    // MUST be static (not stack): 32 KB on a 96 KB kernel stack
    // (boot/boot.asm) is a third of the whole stack and would leave
    // too little headroom for the JPEG/other frames below us.
    static uint8_t window[32768];
    int window_pos = 0;

    deflate_init_tables();

    while (!bfinal) {
        // Read block header
        bfinal = br_read_bit(&br);
        int btype = br_read_bits(&br, 2);
        if (btype < 0) return -1;

        if (btype == 0) {
            // Stored block (no compression)
            // Align to byte boundary
            br.bits = 0;
            br.buf = 0;

            int len = br_read_bits(&br, 16);
            int nlen = br_read_bits(&br, 16);
            if (len < 0 || nlen < 0) return -1;
            if ((len ^ nlen) != 0xFFFF) return -1;  // integrity check

            for (int i = 0; i < len && br.pos < br.len && out_pos < out_max; i++) {
                uint8_t byte = br.data[br.pos++];
                out[out_pos++] = byte;
                window[window_pos & 32767] = byte;
                window_pos++;
            }
        } else if (btype == 1 || btype == 2) {
            // Fixed or dynamic Huffman
            HtTree* p_lt = &lt;
            HtTree* p_dt = &dt;

            if (btype == 1) {
                // Fixed Huffman
                if (ht_build(&lt, deflate_lit_len, 288) < 0) return -1;
                if (ht_build(&dt, deflate_dist, 32) < 0) return -1;
            } else {
                // Dynamic Huffman
                int hlit = br_read_bits(&br, 5) + 257;  // literal/length codes
                int hdist = br_read_bits(&br, 5) + 1;    // distance codes
                int hclen = br_read_bits(&br, 4) + 4;    // code length codes

                // Code length alphabet order
                static const int cl_order[] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                int cl_lengths[19] = {0};
                for (int i = 0; i < hclen; i++) {
                    int v = br_read_bits(&br, 3);
                    if (v < 0) return -1;
                    cl_lengths[cl_order[i]] = v;
                }

                // Build code length tree
                if (ht_build(&clt, cl_lengths, 19) < 0) return -1;
                int cl_root = 0;

                // Read literal/length and distance code lengths
                int total_syms = hlit + hdist;
                int sym_lengths[320];  // 288 + 32
                int sym_count = 0;
                while (sym_count < total_syms) {
                    int sym = ht_decode(&clt, cl_root, &br);
                    if (sym < 0) return -1;

                    if (sym < 16) {
                        sym_lengths[sym_count++] = sym;
                    } else if (sym == 16) {
                        int repeat = br_read_bits(&br, 2) + 3;
                        int prev = sym_count > 0 ? sym_lengths[sym_count - 1] : 0;
                        for (int i = 0; i < repeat && sym_count < total_syms; i++)
                            sym_lengths[sym_count++] = prev;
                    } else if (sym == 17) {
                        int repeat = br_read_bits(&br, 3) + 3;
                        for (int i = 0; i < repeat && sym_count < total_syms; i++)
                            sym_lengths[sym_count++] = 0;
                    } else if (sym == 18) {
                        int repeat = br_read_bits(&br, 7) + 11;
                        for (int i = 0; i < repeat && sym_count < total_syms; i++)
                            sym_lengths[sym_count++] = 0;
                    }
                }

                // Build literal/length and distance trees
                int ll_len[288], dist_len[32];
                for (int i = 0; i < 288; i++) ll_len[i] = 0;
                for (int i = 0; i < 32; i++) dist_len[i] = 0;
                for (int i = 0; i < hlit && i < 288; i++) ll_len[i] = sym_lengths[i];
                for (int i = 0; i < hdist && i < 32; i++) dist_len[i] = sym_lengths[hlit + i];

                if (ht_build(&lt, ll_len, 288) < 0) return -1;
                if (ht_build(&dt, dist_len, 32) < 0) return -1;
            }

            // Decode symbols
            while (out_pos < out_max) {
                int sym = ht_decode(p_lt, 0, &br);
                if (sym < 0) return -1;

                if (sym < 256) {
                    // Literal byte
                    out[out_pos++] = (uint8_t)sym;
                    window[window_pos & 32767] = (uint8_t)sym;
                    window_pos++;
                } else if (sym == 256) {
                    // End of block
                    break;
                } else {
                    // Length (sym 257-285)
                    int len_idx = sym - 257;
                    if (len_idx < 0 || len_idx >= 29) return -1;
                    int length = len_base[len_idx];
                    if (len_extra[len_idx] > 0)
                        length += br_read_bits(&br, len_extra[len_idx]);

                    int dist_idx = ht_decode(p_dt, 0, &br);
                    if (dist_idx < 0 || dist_idx >= 30) return -1;
                    int distance = dist_base[dist_idx];
                    if (dist_extra[dist_idx] > 0)
                        distance += br_read_bits(&br, dist_extra[dist_idx]);

                    // Copy backreference
                    for (int i = 0; i < length && out_pos < out_max; i++) {
                        int src_pos = (window_pos - distance) & 32767;
                        uint8_t byte = window[src_pos];
                        out[out_pos++] = byte;
                        window[window_pos & 32767] = byte;
                        window_pos++;
                    }
                }
            }
        } else {
            return -1; // invalid block type
        }
    }
    return out_pos;
}

// ─── PNG chunk reading helpers ───────────────────────────────────
static uint32_t read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

// ─── PNG filter reversal ─────────────────────────────────────────
static uint8_t paeth_predictor(uint8_t a, uint8_t b, uint8_t c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// ─── PNG decode ──────────────────────────────────────────────────
int png_decode(const uint8_t* data, int datalen, PngImage* img) {
    if (!data || !img || datalen < 33) return 0;

    // Check PNG signature
    static const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    for (int i = 0; i < 8; i++)
        if (data[i] != png_sig[i]) return 0;

    int pos = 8;
    int width = 0, height = 0, bit_depth = 0, color_type = 0;
    // MUST be static (not stack)! Kernel stack is only 96 KB
    // (boot/boot.asm), and IDAT data can be many KB. Stack
    // allocation would immediately overflow and corrupt memory.
    static uint8_t idat_data[65536];
    int idat_len = 0;
    int has_ihdr = 0;

    // Palette data (for color type 3)
    uint8_t palette[256][3];  // RGB triplets
    int palette_entries = 0;
    int palette_transparent = -1;  // index of transparent palette entry, -1 = none

    while (pos + 12 <= datalen) {
        uint32_t chunk_len = read_u32_be(data + pos);
        uint32_t chunk_type;
        chunk_type = ((uint32_t)data[pos+4] << 24) |
                     ((uint32_t)data[pos+5] << 16) |
                     ((uint32_t)data[pos+6] << 8)  |
                     (uint32_t)data[pos+7];

        uint32_t full_chunk_len = 12 + chunk_len;  // len + type + data + crc

        if (pos + full_chunk_len > (uint32_t)datalen) return 0;

        if (chunk_type == 0x49484452) {  // "IHDR"
            if (chunk_len < 13) return 0;
            const uint8_t* ihdr = data + pos + 8;
            width     = (int)read_u32_be(ihdr);
            height    = (int)read_u32_be(ihdr + 4);
            bit_depth = ihdr[8];
            color_type = ihdr[9];
            // ihdr[10] = compression method (must be 0)
            // ihdr[11] = filter method (must be 0)
            // ihdr[12] = interlace method (0=none, 1=Adam7)

            if (ihdr[10] != 0 || ihdr[11] != 0) return 0;  // unsupported compression/filter
            if (ihdr[12] != 0) return 0;  // no interlacing
            if (bit_depth != 8) return 0;
            if (color_type != 2 && color_type != 3 && color_type != 6) return 0;  // RGB, indexed, or RGBA

            has_ihdr = 1;

        } else if (chunk_type == 0x504C5445) {  // "PLTE" — palette
            if (chunk_len > 256 * 3) return 0;
            palette_entries = (int)chunk_len / 3;
            if (palette_entries < 1 || palette_entries > 256) return 0;
            const uint8_t* pd = data + pos + 8;
            for (int i = 0; i < palette_entries; i++) {
                palette[i][0] = pd[i * 3 + 0];
                palette[i][1] = pd[i * 3 + 1];
                palette[i][2] = pd[i * 3 + 2];
            }

        } else if (chunk_type == 0x74524E53) {  // "tRNS" — transparency
            if (color_type == 3 && chunk_len > 0 && (int)chunk_len <= palette_entries) {
                const uint8_t* td = data + pos + 8;
                for (int i = 0; i < (int)chunk_len; i++) {
                    if (td[i] == 0) {
                        palette_transparent = i;
                        break;
                    }
                }
            }

        } else if (chunk_type == 0x49444154) {  // "IDAT"
            const uint8_t* chunk_data = data + pos + 8;
            int to_copy = (int)chunk_len;
            if (idat_len + to_copy > 65536) to_copy = 65536 - idat_len;
            for (int i = 0; i < to_copy; i++)
                idat_data[idat_len++] = chunk_data[i];

        } else if (chunk_type == 0x49454E44) {  // "IEND"
            break;
        }

        pos += (int)full_chunk_len;
    }

    if (!has_ihdr || idat_len == 0) return 0;
    if (width <= 0 || height <= 0) return 0;
    if (width > 4096 || height > 4096) return 0;

    // Bytes per pixel for filter scanning
    int bpp = (color_type == 6) ? 4 : (color_type == 3 ? 1 : 3);
    int scanline_raw = width * bpp;
    int filter_extra = 1;  // filter byte per row
    int inflated_size = (scanline_raw + filter_extra) * height;
    // Same reasoning as idat_data above — kernel stack can't hold this.
    static uint8_t inflated[262144];
    if (inflated_size > (int)sizeof(inflated)) return 0;

    // Strip zlib wrapper: 2-byte header (CMF+FLG) + 4-byte Adler-32 trailer
    int zl_start = 2;
    int zl_len = idat_len - 6;
    if (zl_len < 0) zl_len = 0;
    int inflated_len = png_inflate(idat_data + zl_start, zl_len, inflated, (int)sizeof(inflated));
    if (inflated_len != inflated_size) return 0;

    // Apply PNG filters and convert to RGBA
    uint8_t* prev_row = NULL;
    uint8_t* src = inflated;

    img->width = (uint32_t)width;
    img->height = (uint32_t)height;
    // NOTE: img->pixels must be set by the caller before png_decode().

    for (int y = 0; y < height; y++) {
        int filter_type = *src++;
        uint8_t* cur_row = src;

        // Apply filter
        if (filter_type == 0) {
            // None: bytes are raw
        } else if (filter_type == 1) {
            // Sub: byte += byte from previous pixel in same row
            for (int x = bpp; x < scanline_raw; x++)
                cur_row[x] += cur_row[x - bpp];
        } else if (filter_type == 2) {
            // Up: byte += byte from same position in previous row
            if (prev_row) {
                for (int x = 0; x < scanline_raw; x++)
                    cur_row[x] += prev_row[x];
            }
        } else if (filter_type == 3) {
            // Average: byte += (prev + up) / 2
            for (int x = 0; x < scanline_raw; x++) {
                uint8_t a = (x >= bpp) ? cur_row[x - bpp] : 0;
                uint8_t b = prev_row ? prev_row[x] : 0;
                cur_row[x] += (a + b) / 2;
            }
        } else if (filter_type == 4) {
            // Paeth
            for (int x = 0; x < scanline_raw; x++) {
                uint8_t a = (x >= bpp) ? cur_row[x - bpp] : 0;
                uint8_t b = prev_row ? prev_row[x] : 0;
                uint8_t c = (prev_row && x >= bpp) ? prev_row[x - bpp] : 0;
                cur_row[x] += paeth_predictor(a, b, c);
            }
        } else {
            return 0;  // unknown filter
        }

        // Convert to RGBA in the output buffer
        uint8_t* dst = img->pixels + y * width * 4;
        if (color_type == 3) {
            // Indexed/palette: each byte is a palette index
            for (int x = 0; x < width; x++) {
                int idx = cur_row[x];
                if (idx >= palette_entries) idx = 0;
                dst[x * 4 + 0] = palette[idx][0];  // R
                dst[x * 4 + 1] = palette[idx][1];  // G
                dst[x * 4 + 2] = palette[idx][2];  // B
                dst[x * 4 + 3] = (idx == palette_transparent) ? 0 : 255;  // A
            }
        } else {
            for (int x = 0; x < width; x++) {
                dst[x * 4 + 0] = cur_row[x * bpp + 0];  // R
                dst[x * 4 + 1] = cur_row[x * bpp + 1];  // G
                dst[x * 4 + 2] = cur_row[x * bpp + 2];  // B
                dst[x * 4 + 3] = (bpp == 4) ? cur_row[x * bpp + 3] : 255;  // A
            }
        }

        prev_row = cur_row;
        src += scanline_raw;
    }

    return 1;
}
