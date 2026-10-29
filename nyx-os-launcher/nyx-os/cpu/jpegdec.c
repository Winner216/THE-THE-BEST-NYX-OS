// cpu/jpegdec.c — Minimal JPEG decoder for Nyx OS
//
// Supported subset:
//   - Baseline DCT (SOF0) only
//   - YCbCr color space with 4:2:0 or 4:4:4 subsampling
//   - Up to 4 Huffman tables (2 DC + 2 AC)
//   - Up to 4 quantization tables
//   - Single scan (interleaved)
//
// Limitations:
//   - No progressive JPEG, no arithmetic coding
//   - No restart markers, no grayscale

#include <jpegdec.h>
#include <stdint.h>
#include <stddef.h>

// ─── Safety limits (validated BEFORE any output is written) ──────
// Every caller buffer is 1920×1080×4: gallery's pixbuf, the Files
// app's fm_pixbuf, and vesa.draw (the zero-copy video path decodes
// straight into the framebuffer).  The decoder therefore refuses
// anything larger — even when a caller forgets its own pre-check,
// a hostile header can never drive a write past the caller's buffer.
#define JPEG_MAX_W  1920
#define JPEG_MAX_H  1080
// Per-component block capacity of the stack MCU buffer comp_out
// (64*4 bytes = 4 blocks).  JPEG allows sampling factors up to 4, but
// a crafted 4×4 factor would write ~823 bytes into the 256-byte
// buffer; cap total blocks per component at 4 (covers 4:4:4 and the
// 4:2:0 luma 2×2 case).
#define JPEG_MAX_BLOCKS  4

// ─── Bit reader (MSB-first; JPEG bit order) ──────────────────────
typedef struct {
    const uint8_t* data;
    int            len;
    int            pos;
    int            bits_remaining;
    uint8_t        byte_buf;
} JBitReader;

static void jbr_init(JBitReader* br, const uint8_t* d, int len) {
    br->data = d; br->len = len; br->pos = 0;
    br->bits_remaining = 0; br->byte_buf = 0;
}

static int jbr_read_bit(JBitReader* br) {
    if (br->bits_remaining == 0) {
        if (br->pos >= br->len) return -1;
        br->byte_buf = br->data[br->pos++];
        if (br->byte_buf == 0xFF && br->pos < br->len && br->data[br->pos] == 0x00)
            br->pos++;
        br->bits_remaining = 8;
    }
    br->bits_remaining--;
    return (br->byte_buf >> br->bits_remaining) & 1;
}

static int jbr_read_bits(JBitReader* br, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bit = jbr_read_bit(br);
        if (bit < 0) return -1;
        v = (v << 1) | bit;
    }
    return v;
}

// ─── Huffman tree (tree-based) ───────────────────────────────────
// Node budget per Huffman table.  A full JPEG AC table has 162
// symbols; the canonical prefix tree built from it needs up to 336
// nodes in the worst realistic case (gstreamer/jpegenc and the
// standard Annex K tables both need exactly 324; verified by
// tools/_ht_budget.py).  256 was too small and made jht_build()
// fail — breaking every frame.  512 leaves a generous margin at a
// cost of only 512*6 = 3 KB per table.
#define JPEG_HT_NODES 512

// Packed 6-byte node (NOT padded to 8): the trie's children are
// written at adjacent indices, so 6-byte nodes keep parent→child
// hops ~6 bytes apart — tighter locality than a padded struct, and
// the array stays 3 KB per table instead of 4 KB (512 nodes × 6 B).
// The JHtTree itself is 64 B-aligned, which is what keeps the array
// base on a cache-line boundary.
typedef struct {
    int16_t kids[2];
    int16_t sym;
} JHtNode;

typedef struct {
    JHtNode nodes[JPEG_HT_NODES];
    int     count;
} JHtTree;

static int jht_new_node(JHtTree* t) {
    if (t->count >= JPEG_HT_NODES) return -1;
    int idx = t->count++;
    t->nodes[idx].kids[0] = -1;
    t->nodes[idx].kids[1] = -1;
    t->nodes[idx].sym = -1;
    return idx;
}

static int jht_build(JHtTree* t, const uint8_t bits[16], const uint8_t* values, int num_values) {
    t->count = 0;
    int root = jht_new_node(t);
    if (root < 0) return -1;
    int code = 0, vi = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1] && vi < num_values; i++) {
            int sym = values[vi++];
            int node = root;
            for (int b = len - 1; b >= 0; b--) {
                int bit = (code >> b) & 1;
                if (t->nodes[node].kids[bit] < 0) {
                    int n = jht_new_node(t);
                    if (n < 0) return -1;
                    t->nodes[node].kids[bit] = (int16_t)n;
                }
                node = t->nodes[node].kids[bit];
            }
            t->nodes[node].sym = (int16_t)sym;
            code++;
        }
        code <<= 1;
    }
    return root;
}

static int jht_decode(JHtTree* t, int root, JBitReader* br) {
    int node = root;
    while (t->nodes[node].sym < 0) {
        int bit = jbr_read_bit(br);
        if (bit < 0) return -1;
        int kid = t->nodes[node].kids[bit];
        if (kid < 0) return -1;
        node = kid;
    }
    return t->nodes[node].sym;
}

// ─── IDCT: row-column 1D IDCT with fixed-point arithmetic ───────
// cos((2*x+1)*u*pi/16) * 65536 for x=0..7, u=0..7
static const int idct_cos[8][8] = {
    { 65536,  64277,  60547,  54491,  46341,  36407,  25080,  12785 },
    { 65536,  54491,  25080, -12785, -46341, -64277, -60547, -36407 },
    { 65536,  36407, -25080, -64277, -46341,  12785,  60547,  54491 },
    { 65536,  12785, -60547, -36407,  46341,  54491, -25080, -64277 },
    { 65536, -12785, -60547,  36407,  46341, -54491, -25080,  64277 },
    { 65536, -36407, -25080,  64277, -46341, -12785,  60547, -54491 },
    { 65536, -54491,  25080,  12785, -46341,  64277, -60547,  36407 },
    { 65536, -64277,  60547, -54491,  46341, -36407,  25080, -12785 },
};

// C(0) = 1/sqrt(2) * 65536 = 46341, C(n) = 65536 for n>0
/* idct_cf unused — row-column IDCT handles C(u) inline */
/* static const int idct_cf[8] = { 46341, 65536, 65536, 65536, 65536, 65536, 65536, 65536 }; */

static void idct_2d(const int* coeffs, uint8_t* output) {
    // De-zigzag
    static const int zig[64] = {
         0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
    };
    int blk[8][8];
    for (int i = 0; i < 64; i++) blk[zig[i]/8][zig[i]%8] = coeffs[i];

    // Row-column IDCT.  GCC -O2 converts / 65536 and / 262144
    // to arithmetic shifts automatically.
    int tmp[8][8];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int64_t sum = (int64_t)blk[y][0] * 46341;
            for (int u = 1; u < 8; u++)
                sum += (int64_t)blk[y][u] * idct_cos[x][u];
            tmp[y][x] = (int)(sum / 65536);
        }
    }
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            int64_t sum = (int64_t)tmp[0][x] * 46341;
            for (int v = 1; v < 8; v++)
                sum += (int64_t)tmp[v][x] * idct_cos[y][v];
            int val = (int)(sum / 262144) + 128;
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            output[y * 8 + x] = (uint8_t)val;
        }
    }
}

// ─── Bilinear chroma upsampler ───────────────────────────────────
// `plane` is a decoded component grid (8*h wide × 8*v tall blocks);
// `x`/`y` are coordinates in luma MCU space (8*max_h × 8*max_v).
// For 4:2:0 this replaces nearest-neighbour chroma upsampling — the
// single biggest source of the blocky "pixelated" look — with smooth
// bilinear interpolation.  For 4:4:4 (h==max_h, v==max_v) the
// fractional weights are always zero, so it reduces to plain lookup.
static int j_chroma_sample(const uint8_t* plane, int h, int v,
                           int max_h, int max_v, int x, int y) {
    int cw = 8 * h, ch = 8 * v;
    int fx_fp = (x * h * 65536) / max_h;
    int fy_fp = (y * v * 65536) / max_v;
    int x0 = fx_fp >> 16, y0 = fy_fp >> 16;
    if (x0 >= cw) x0 = cw - 1;
    if (y0 >= ch) y0 = ch - 1;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x1 >= cw) x1 = cw - 1;
    if (y1 >= ch) y1 = ch - 1;
    int wx = fx_fp & 0xFFFF, wy = fy_fp & 0xFFFF;
    int iwx = 65536 - wx, iwy = 65536 - wy;
    int v00 = plane[y0*cw + x0];
    int v01 = plane[y0*cw + x1];
    int v10 = plane[y1*cw + x0];
    int v11 = plane[y1*cw + x1];
    int h0 = (v00*iwx + v01*wx + 32768) >> 16;
    int h1 = (v10*iwx + v11*wx + 32768) >> 16;
    return (h0*iwy + h1*wy + 32768) >> 16;
}

// ─── JPEG decode ─────────────────────────────────────────────────
// Shared core.  mode: 0 = full resolution, 1 = VERTICAL half (drop
// odd MCU rows only → output width × height/2, keeping all
// horizontal detail), 2 = half in BOTH axes (drop odd rows AND
// columns → output width/2 × height/2).  In ECO modes only kept
// MCUs are rendered; each kept MCU tiles the decimated output
// exactly, so the grid has no gaps or stale pixels.  The entropy
// stream is still consumed for ALL MCUs — Huffman decoding and DC
// prediction are strictly sequential — but dequantize + IDCT +
// YCbCr→RGB (the expensive parts) are skipped for dropped MCUs,
// giving ~2× less render work for vertical-half and ~3-4× for
// both-axes on 576×576 frames.
//
// Not static: cpu/jpegasync.c calls it from the background decode
// worker task (see jpegdec.h).  All header/dimension validation
// above runs before any output byte is written.
int jpeg_decode_inner(const uint8_t* data, int datalen,
                      JpegImage* img, int mode) {
    if (!data || !img || datalen < 2) return 0;
    if (data[0] != 0xFF || data[1] != 0xD8) return 0;

    int pos = 2;
    int width = 0, height = 0, num_components = 0;
    int comp_id[4] = {0}, comp_h[4] = {0}, comp_v[4] = {0};
    uint8_t quant_tables[4][64];
    int quant_present[4] = {0};
    // Indexing: DC table tid → tid; AC table tid → tid+2 (DHT stores
    // both classes in one array).  So the arrays must hold indices 0..5
    // (AC table ids 2/3 — used by the K plane of 4-color files — would
    // otherwise write past a [4] array).
    JHtTree ht_tables[6];
    int ht_roots[6] = {0}, ht_present[6] = {0};
    int comp_dc_tbl[4] = {0}, comp_ac_tbl[4] = {0};
    int max_h = 0, max_v = 0, scan_start = 0;
    // Adobe APP14 color transform (-1 = marker absent): 0 = plain CMYK,
    // 1 = YCbCr, 2 = YCCK.  Only meaningful for 4-component files.
    int adobe_transform = -1;

    while (pos + 1 < datalen) {
        if (data[pos] != 0xFF) { pos++; continue; }
        while (pos < datalen && data[pos] == 0xFF) pos++;
        if (pos >= datalen) break;
        uint8_t marker = data[pos++];
        if (marker == 0xD9) break;
        if (marker == 0x00 || marker == 0xD8) continue;
        if (pos + 1 >= datalen) break;
        int seg_len = ((int)data[pos] << 8) | (int)data[pos + 1];
        if (seg_len < 2) continue;
        int seg_end = pos + seg_len;
        if (seg_end > datalen) break;

        if (marker == 0xDB) {  // DQT
            int qpos = pos + 2;
            while (qpos + 65 <= seg_end) {
                int info = data[qpos++];
                int tid = info & 0x0F;
                if (tid > 3) break;
                for (int i = 0; i < 64; i++)
                    quant_tables[tid][i] = data[qpos++];
                quant_present[tid] = 1;
            }
        } else        if (marker == 0xC0) {  // SOF0
            if (pos + 8 >= datalen || data[pos+2] != 8) return 0;
            height = ((int)data[pos+3]<<8)|data[pos+4];
            width  = ((int)data[pos+5]<<8)|data[pos+6];
            num_components = data[pos+7];
            // ── Header metadata validation (memory safety) ────────
            // Bounds on width AND height before any pixel is written:
            // the decode loops iterate width×height MCUs and write
            // into the caller's buffer — an unbounded width (the old
            // code only capped height at 4096) would overflow it.
            if ((num_components != 3 && num_components != 4) ||
                width <= 0 || width > JPEG_MAX_W ||
                height <= 0 || height > JPEG_MAX_H) return 0;
            for (int i = 0; i < num_components; i++) {
                int ci = pos + 8 + i*3;
                comp_id[i] = data[ci];
                comp_h[i] = (data[ci+1]>>4)&0x0F;
                comp_v[i] = data[ci+1]&0x0F;
                comp_dc_tbl[i] = data[ci+2];
                // Sampling-factor validation: each factor 1..4 and
                // h*v <= 4 so comp_out[c] (256 bytes = 4 blocks)
                // can never be overrun, and max_h/max_v <= 4 keeps
                // the MCU tile <= 32×32.
                if (comp_h[i] < 1 || comp_h[i] > 4 ||
                    comp_v[i] < 1 || comp_v[i] > 4 ||
                    comp_h[i] * comp_v[i] > JPEG_MAX_BLOCKS) return 0;
                if (comp_h[i] > max_h) max_h = comp_h[i];
                if (comp_v[i] > max_v) max_v = comp_v[i];
            }
        } else if (marker == 0xEE) {  // APP14 — Adobe color transform
            // 12-byte payload: version(2) flags0(2) flags1(2) transform(1)
            // transform byte is payload offset 11.  seg_len includes the
            // 2 length bytes, so it must be >= 14 to contain it.
            if (seg_len >= 14)
                adobe_transform = data[pos + 2 + 11];
        } else if (marker == 0xC4) {  // DHT
            int hp = pos + 2;
            while (hp + 17 <= seg_end) {
                int info = data[hp++];
                int cls = (info>>4)&0x0F, tid = info&0x0F;
                if (tid > 3) break;
                int idx = tid + (cls ? 2 : 0);
                uint8_t bits[16]; int ns = 0;
                for (int i = 0; i < 16; i++) { bits[i]=data[hp++]; ns+=bits[i]; }
                // A malformed table can declare more code values than
                // the segment holds — never read past seg_end.
                if (hp + ns > seg_end) return 0;
                ht_roots[idx] = jht_build(&ht_tables[idx], bits, data+hp, ns);
                ht_present[idx] = 1;
                hp += ns;
            }
        } else if (marker == 0xDA) {  // SOS
            int nsc = data[pos+2];
            if (nsc < 1 || nsc > 4) break;
            for (int i = 0; i < nsc; i++) {
                int si = pos + 3 + i*2;
                int sid = data[si];
                for (int j=0;j<num_components;j++)
                    if (comp_id[j]==sid) {
                        comp_dc_tbl[j]=(data[si+1]>>4)&0x0F;
                        comp_ac_tbl[j]=data[si+1]&0x0F;
                    }
            }
            // Table selectors are 4-bit nibbles (0..15), but the
            // decode path indexes ht_tables[dc + 0..5] and
            // ht_tables[ac + 2] — clamp to the 4 supported tables
            // BEFORE any verification/decode uses them.
            for (int j = 0; j < num_components; j++)
                if (comp_dc_tbl[j] > 3 || comp_ac_tbl[j] > 3) return 0;
            scan_start = pos + 3 + nsc*2 + 3;
        }
        pos = seg_end;
    }

    if (width<=0 || (num_components!=3 && num_components!=4) ||
        scan_start<=0) return 0;

    // 4-component files: plain CMYK when Adobe APP14 says transform 0,
    // otherwise YCCK (the libjpeg default — far more common than plain
    // CMYK in real CMYK JPEGs).
    int cmyk_plain = (num_components == 4 && adobe_transform == 0);

    // Verify Huffman tables
    for (int i=0;i<num_components;i++)
        if (!ht_present[comp_dc_tbl[i]+0] || !ht_present[comp_ac_tbl[i]+2]) return 0;

    // Quantization tables: Y uses table 0. Many JPEGs only provide
    // a single table (index 0) and use it for both Y and CbCr.
    if (!quant_present[0]) return 0;
    if (!quant_present[1]) {
        for (int _q = 0; _q < 64; _q++) quant_tables[1][_q] = quant_tables[0][_q];
        quant_present[1] = 1;
    }

    JBitReader br;
    jbr_init(&br, data + scan_start, datalen - scan_start);

    int mcu_w = 8*max_h, mcu_h = 8*max_v;
    int mcu_cx = (width+mcu_w-1)/mcu_w, mcu_cy = (height+mcu_h-1)/mcu_h;

    // ECO output geometry — keep ceil(rows/2) × ceil(cols/2) MCUs,
    // each writing a full block into the half-size grid, so output is
    // fully tiled (no gaps).  For odd source dims the last kept MCU
    // column/row writes a few in-bounds pixels past the true half
    // width/height — harmless, the caller blits by img->width/height.
    int out_w = width, out_h = height;
    if (mode >= 1) out_h = ((mcu_cy + 1) / 2) * mcu_h;  // vertical half
    if (mode >= 2) out_w = ((mcu_cx + 1) / 2) * mcu_w;  // horizontal half too
    img->width = (uint32_t)out_w;
    img->height = (uint32_t)out_h;

    // Output row stride — 0 means tightly packed (width*4).  A caller
    // stride lets the video player decode straight into the framebuffer
    // draw buffer (vesa.pitch) with no intermediate RGBA copy — the
    // zero-copy path in cpu/video.c.
    int row_stride = img->stride ? img->stride : (int)(out_w * 4);
    int prev_dc[4] = {0};

    for (int my = 0; my < mcu_cy; my++) {
        for (int mx = 0; mx < mcu_cx; mx++) {
            uint8_t comp_out[4][64*4];  // worst case: 4 blocks per component
            int render = 1;
            if (mode == 1) render = ((my & 1) == 0);               // vertical only
            else if (mode == 2) render = ((my & 1) == 0) && ((mx & 1) == 0);

            for (int c = 0; c < num_components; c++) {
                int h = comp_h[c], v = comp_v[c];
                int dc_idx = comp_dc_tbl[c];
                int ac_idx = comp_ac_tbl[c] + 2;

                for (int by = 0; by < v; by++) {
                    for (int bx = 0; bx < h; bx++) {
                        int coeffs[64] = {0};
                        int cat = jht_decode(&ht_tables[dc_idx], ht_roots[dc_idx], &br);
                        if (cat < 0 || cat > 11) return 0;
                        int diff = 0;
                        if (cat > 0) {
                            int bv = jbr_read_bits(&br, cat);
                            if (bv < 0) return 0;
                            diff = (bv & (1<<(cat-1))) ? bv : bv - ((1<<cat)-1);
                        }
                        prev_dc[c] += diff;
                        coeffs[0] = prev_dc[c];

                        int k = 1;
                        while (k < 64) {
                            int sym = jht_decode(&ht_tables[ac_idx], ht_roots[ac_idx], &br);
                            if (sym < 0) return 0;
                            if (sym == 0) break;
                            int run = (sym>>4)&0x0F, cat2 = sym&0x0F;
                            if (run==15 && cat2==0) { k+=16; continue; }
                            k += run;
                            if (k >= 64) break;
                            int bv = jbr_read_bits(&br, cat2);
                            if (bv < 0) return 0;
                            coeffs[k] = (bv & (1<<(cat2-1))) ? bv : bv - ((1<<cat2)-1);
                            k++;
                        }

                        // Dequantize + IDCT — skipped for dropped MCUs
                        // in ECO mode (the bits were already consumed
                        // above, so DC prediction and bit position stay
                        // in sync with the encoder's raster order).
                        if (!render) continue;
                        int qid = (c==0) ? 0 : 1;
                        int dq[64];
                        for (int i=0;i<64;i++) dq[i] = coeffs[i] * (int)quant_tables[qid][i];

                        uint8_t bp[64];
                        idct_2d(dq, bp);

                        int bw = 8*h, row_off = by*8 + bx*8/bw;
                        int col_off = bx*8;
                        for (int pi=0;pi<64;pi++)
                            comp_out[c][row_off*bw + col_off + (pi/8)*bw + pi%8] = bp[pi];
                    }
                }
            }

            // Combine planes → sRGB (dropped MCUs never reach here).
            // ECO modes write each kept MCU into the decimated grid:
            // vertical-only at (my>>1, mx) — full horizontal detail —
            // and both-axes at (my>>1, mx>>1); exact 2:1 sampling.
            if (!render) continue;
            int omy = (mode >= 1) ? (my >> 1) : my;
            int omx = (mode >= 2) ? (mx >> 1) : mx;
            for (int y = 0; y < mcu_h; y++) {
                int iy = omy*mcu_h + y;
                if (iy >= out_h) break;
                for (int x = 0; x < mcu_w; x++) {
                    int ix = omx*mcu_w + x;
                    if (ix >= out_w) break;

                    uint8_t* dst = img->pixels + (size_t)iy*row_stride + ix*4;
                    if (num_components == 3) {
                        // ── YCbCr → sRGB (integer BT.601 math) ─────
                        // Luma is 1:1 (comp_h[0]==max_h,
                        // comp_v[0]==max_v), so direct grid indexing.
                        int Y = comp_out[0][y*(8*max_h) + x];

                        // Chroma — bilinear upsample (removes 4:2:0
                        // blockiness; identity for 4:4:4).
                        int Cb = j_chroma_sample(comp_out[1], comp_h[1], comp_v[1],
                                                 max_h, max_v, x, y);
                        int Cr = j_chroma_sample(comp_out[2], comp_h[2], comp_v[2],
                                                 max_h, max_v, x, y);

                        int cb = Cb - 128, cr = Cr - 128;
                        int r = Y + (cr*1436+512)/1024;
                        int g = Y - (cb*352+512)/1024 - (cr*731+512)/1024;
                        int b = Y + (cb*1815+512)/1024;

                        if (r<0) r=0; else if (r>255) r=255;
                        if (g<0) g=0; else if (g>255) g=255;
                        if (b<0) b=0; else if (b>255) b=255;

                        dst[0]=(uint8_t)r; dst[1]=(uint8_t)g;
                        dst[2]=(uint8_t)b; dst[3]=255;
                    } else {
                        // ── 4-color: YCCK or plain CMYK → sRGB ─────
                        // Both store the Adobe convention: 0 = max ink,
                        // 255 = no ink.  Plain CMYK (APP14 transform 0)
                        // stores C/M/Y/K directly; YCCK (transform 2,
                        // libjpeg default for 4-color) stores the image's
                        // YCbCr plus an inverted K plane (per GCR).
                        //
                        // libjpeg's ycck_cmyk_convert maps YCCK to
                        //   C = 255 - R_ycc   (positive ink)
                        //   K passes through  (file convention)
                        // and apps finish with additive subtractive
                        //   R = 255 - min(255, C_ink + K_ink)
                        // Both branches collapse to the same exact form
                        //   R = max(0, C + K - 255)
                        // where C is the stored cyan (plain CMYK) or
                        // R_ycc (YCCK).  Verified pixel-perfect against
                        // PIL for plain CMYK, and exact for GCR YCCK
                        // (R_ycc = 255 - C_after, so the K subtracts
                        // back out to the true red).
                        int K = j_chroma_sample(comp_out[3], comp_h[3], comp_v[3],
                                                max_h, max_v, x, y);
                        int r, g, b;
                        if (cmyk_plain) {
                            int C  = j_chroma_sample(comp_out[0], comp_h[0], comp_v[0],
                                                     max_h, max_v, x, y);
                            int M  = j_chroma_sample(comp_out[1], comp_h[1], comp_v[1],
                                                     max_h, max_v, x, y);
                            int Yk = j_chroma_sample(comp_out[2], comp_h[2], comp_v[2],
                                                     max_h, max_v, x, y);
                            r = C + K - 255; if (r < 0) r = 0;
                            g = M + K - 255; if (g < 0) g = 0;
                            b = Yk + K - 255; if (b < 0) b = 0;
                        } else {
                            // YCCK: YCbCr→RGB like the 3-component path,
                            // then subtract the black ink.
                            int Y  = comp_out[0][y*(8*max_h) + x];
                            int Cb = j_chroma_sample(comp_out[1], comp_h[1], comp_v[1],
                                                     max_h, max_v, x, y);
                            int Cr = j_chroma_sample(comp_out[2], comp_h[2], comp_v[2],
                                                     max_h, max_v, x, y);
                            int cb = Cb - 128, cr = Cr - 128;
                            r = Y + (cr*1436+512)/1024;
                            g = Y - (cb*352+512)/1024 - (cr*731+512)/1024;
                            b = Y + (cb*1815+512)/1024;
                            if (r<0) r=0; else if (r>255) r=255;
                            if (g<0) g=0; else if (g>255) g=255;
                            if (b<0) b=0; else if (b>255) b=255;
                            r = r + K - 255; if (r < 0) r = 0;
                            g = g + K - 255; if (g < 0) g = 0;
                            b = b + K - 255; if (b < 0) b = 0;
                        }
                        dst[0]=(uint8_t)r; dst[1]=(uint8_t)g;
                        dst[2]=(uint8_t)b; dst[3]=255;
                    }
                }
            }
        }
    }
    return 1;
}

int jpeg_decode(const uint8_t* data, int datalen, JpegImage* img) {
    return jpeg_decode_inner(data, datalen, img, 0);
}

// Vertical-only ECO — keeps every column, drops odd MCU rows, so the
// output is width × height/2 (288×576 for a 576² frame).  Roughly 2×
// faster than full decode while retaining full horizontal detail —
// the intermediate rung of the player's adaptive ladder.
int jpeg_decode_eco_v(const uint8_t* data, int datalen, JpegImage* img) {
    return jpeg_decode_inner(data, datalen, img, 1);
}

// Both-axes ECO — drops odd rows AND columns → width/2 × height/2
// (288×288 for a 576² frame).  ~3-4× faster; the final rung used
// only when still over budget after vertical-half.
int jpeg_decode_eco(const uint8_t* data, int datalen, JpegImage* img) {
    return jpeg_decode_inner(data, datalen, img, 2);
}

// ─── Light deblocking (JPEG MCU grid) ────────────────────────────
// Simplified H.264-style weak filter applied ONLY at the 8×8 block
// boundaries on both axes; block interiors are never touched, so the
// picture content is preserved and the cost stays small (≈2 pixels
// per 8-pixel edge).  Luma (r+g+b sums) drives both the decision and
// the delta; the same per-channel delta is applied to R, G and B so
// hue is preserved (no color fringing).
//
//   Decision: filter a boundary pixel pair (p0 above/left, q0
//   below/right, with second neighbours p1/q1) when
//     |p0−q0| < ALPHA   — a moderate step, not a real object edge
//     |p1−p0| < BETA ∧ |q1−q0| < BETA — both sides locally flat
//   Real edges (large steps) and texture (steep local slopes) are
//   left alone; only flat-to-flat 1-2 LSB steps — exactly the JPEG
//   block grid — get evened out.
//   Delta: the H.264 weak-filter blend clamped to ±TC, a small
//   per-pixel correction that halves the visible step.
//   Note: a pixel at a grid CROSS (both an MCU row and column
//   boundary) is visited by both passes and can move up to 2×TC — the
//   same corner double-filtering the H.264 spec filter has.  A tiny
//   minority of steps may then grow by ≤3 when the two passes
//   disagree slightly; accepted and bounded.
#define JPEG_DB_ALPHA  20      // max luma step treated as artifact
#define JPEG_DB_BETA   12      // max local slope on either side
#define JPEG_DB_TC      4      // max per-pixel correction
#define JPEG_DB_TC_STRONG 6    // strong filter for large steps

// One boundary pixel pair: decide from the luma sums (p1 p0 q0 q1 are
// the 4 pixels straddling an 8-px MCU boundary), then correct each
// channel with its own weak-filter delta.  static inline so both the
// horizontal and vertical passes share identical logic (inlined at
// -O2, so no hot-loop call cost) and can never drift apart.
//
// UPDATED: raised thresholds for video MJPEG at low quality.
// Added strong-filter path for large steps (dp0 > alpha): when the
// boundary step is moderate (not a real edge but larger than the
// weak filter can fix), push harder with up to TC_STRONG correction
// and also nudge p1/q1 to widen the transition band.
static inline void jpeg_db_pair(uint8_t* p1, uint8_t* p0,
                                uint8_t* q0, uint8_t* q1) {
    int sp1 = p1[0] + p1[1] + p1[2], sp0 = p0[0] + p0[1] + p0[2];
    int sq0 = q0[0] + q0[1] + q0[2], sq1 = q1[0] + q1[1] + q1[2];
    int dp0 = sp0 - sq0; if (dp0 < 0) dp0 = -dp0;
    int dp1 = sp1 - sp0; if (dp1 < 0) dp1 = -dp1;
    int dq1 = sq1 - sq0; if (dq1 < 0) dq1 = -dq1;
    if (dp0 >= JPEG_DB_ALPHA * 3 ||
        dp1 >= JPEG_DB_BETA  * 3 || dq1 >= JPEG_DB_BETA * 3)
        return;                              // real edge or texture — keep

    // Choose weak or strong filter based on the boundary step magnitude.
    int tc = (dp0 > JPEG_DB_ALPHA) ? JPEG_DB_TC_STRONG : JPEG_DB_TC;

    // Per-channel H.264 weak-filter delta, clamped so a channel's
    // step never grows: |d| ≤ min(TC, |diff|/2) keeps the pair from
    // crossing its midpoint (a luma-shared delta could flip a channel
    // whose own step is smaller than the luma step).  Channels are
    // corrected independently by ≤ TC, so hue shifts stay negligible.
    for (int c = 0; c < 3; c++) {
        int diff = q0[c] - p0[c];
        if (diff == 0) continue;
        int d = ((diff << 2) + (p1[c] - q1[c]) + 4) >> 3;
        if ((d > 0) != (diff > 0)) d = 0;   // sign guard: never push away
        if (d >  tc) d =  tc;
        if (d < -tc) d = -tc;
        int lim = diff >> 1;               // never cross midpoint
        int alim = lim < 0 ? -lim : lim;   // magnitude, sign-kept
        if (d >  alim) d =  alim;
        if (d < -alim) d = -alim;
        if (d) {
            int vp = p0[c] + d; if (vp < 0) vp = 0; else if (vp > 255) vp = 255;
            int vq = q0[c] - d; if (vq < 0) vq = 0; else if (vq > 255) vq = 255;
            p0[c] = (uint8_t)vp;
            q0[c] = (uint8_t)vq;
            // Strong filter: also nudge p1/q1 by half the correction
            // to widen the transition and kill the grid pattern.
            if (dp0 > JPEG_DB_ALPHA) {
                int hd = d >> 1;
                if (hd) {
                    int vp1 = p1[c] + hd; if (vp1 < 0) vp1 = 0; else if (vp1 > 255) vp1 = 255;
                    int vq1 = q1[c] - hd; if (vq1 < 0) vq1 = 0; else if (vq1 > 255) vq1 = 255;
                    p1[c] = (uint8_t)vp1;
                    q1[c] = (uint8_t)vq1;
                }
            }
        }
    }
}

// Light 3-tap lowpass on one pixel: blend with its vertical
// neighbors.  p[-1], p[0], p[1] are RGBA pixel pointers.  blend
// factor: p[0] = (p[-1] + 2*p[0] + p[1]) / 4 — a single-tap
// gaussian that kills single-pixel spikes without softening real
// edges.
static inline void jpeg_db_smooth3(uint8_t* prev, uint8_t* cur,
                                   uint8_t* next) {
    for (int c = 0; c < 3; c++) {
        int v = (int)prev[c] + 2 * (int)cur[c] + (int)next[c];
        cur[c] = (uint8_t)((v + 2) >> 2);
    }
}

void jpeg_deblock_rgba(uint8_t* rgba, int w, int h, int stride) {
    if (!rgba || w < 16 || h < 16) return;
    if (stride == 0) stride = w * 4;

    // Pass 1: standard boundary filter (4-pixel stencil at exact 8×8 edges).
    // Horizontal boundaries — between MCU rows.
    for (int y = 8; y < h; y += 8) {
        uint8_t* rp1 = rgba + (size_t)(y - 2) * stride;
        uint8_t* rp0 = rgba + (size_t)(y - 1) * stride;
        uint8_t* rq0 = rgba + (size_t)y      * stride;
        uint8_t* rq1 = rgba + (size_t)(y + 1) * stride;
        for (int x = 0; x < w; x++)
            jpeg_db_pair(rp1 + x * 4, rp0 + x * 4, rq0 + x * 4, rq1 + x * 4);
    }

    // Vertical boundaries — between MCU columns (same filter, transposed).
    for (int x = 8; x < w; x += 8) {
        for (int y = 0; y < h; y++) {
            uint8_t* p1 = rgba + (size_t)y * stride + (size_t)(x - 2) * 4;
            uint8_t* p0 = rgba + (size_t)y * stride + (size_t)(x - 1) * 4;
            uint8_t* q0 = rgba + (size_t)y * stride + (size_t)x * 4;
            uint8_t* q1 = rgba + (size_t)y * stride + (size_t)(x + 1) * 4;
            jpeg_db_pair(p1, p0, q0, q1);
        }
    }

    // NOTE: Pass 2 (inner-MCU smoothing) removed — it was adding
    // excessive blur that made the video look pixelated.  The boundary
    // filter (Pass 1) is sufficient to remove MCU grid artifacts
    // while preserving sharp detail.
}
