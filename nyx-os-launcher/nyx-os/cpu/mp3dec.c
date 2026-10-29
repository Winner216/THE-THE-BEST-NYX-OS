// cpu/mp3dec.c — MPEG-1 Layer III decoder (integer fixed point)
//
// Implements the full ISO/IEC 11172-3 Layer III pipeline.  The DSP math
// follows the structure of the well-tested reference decoder (minimp3,
// public domain) but is ported to pure integer fixed point because this
// kernel has no FPU and no libgcc soft-float.  All trigger coefficients
// are generated to fixed-point formats by tools/gen-mp3tables.py; the
// numeric scale is chosen so the int16 output needs no extra shift.
//
// The four requested pipeline stages map to:
//   bitstream parser   -> mp3_side_info() + header / reservoir assembly
//   huffman decoding   -> mp3_huff_granule() (tries in mp3tables.c)
//   quant/reorder      -> pow43 * 2^(E/4) dequant + mp3_reorder()
//   stereo processing  -> mp3_midside() + mp3_intensity()
// followed by antialias, IMDCT and the 32-band polyphase filterbank.

#include <mp3dec.h>
#include <stddef.h>

// ─── Bit reader (MSB-first) ───────────────────────────────────────
typedef struct {
    const uint8_t* buf;
    int len;          // bytes
    int pos;          // bit position
} BR;

static inline int br_bit(const BR* b, int idx) {
    int bi = idx >> 3;
    if (bi < 0 || bi >= b->len) return 0;
    return (b->buf[bi] >> (7 - (idx & 7))) & 1;
}
static inline int br_get(BR* b, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | br_bit(b, b->pos++);
    return v;
}

// ─── Header ───────────────────────────────────────────────────────
static const int g_bitrate_tab[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
static const int g_sr_tab[4]       = { 44100, 48000, 32000, 0 };

static int mp3_header(const uint8_t* h,
                      int* channels, int* sr, int* br_kbps, int* frame_bytes, int* sr_idx) {
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;   // 11 sync bits
    int version = (h[1] >> 3) & 3;      // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
    int layer   = (h[1] >> 1) & 3;      // 1 = Layer III
    if (version != 3 || layer != 1) return 0;           // MPEG1 Layer III only
    int br_idx = (h[2] >> 4) & 15;
    int sr2    = (h[2] >> 2) & 3;
    int pad    = (h[2] >> 1) & 1;
    int mode   = (h[3] >> 6) & 3;
    if (br_idx == 0 || br_idx == 15 || sr2 == 3) return 0;
    int br = g_bitrate_tab[br_idx];
    int s  = g_sr_tab[sr2];
    if (br <= 0 || s <= 0) return 0;
    if (channels)   *channels    = (mode == 3) ? 1 : 2;
    if (sr)         *sr          = s;
    if (br_kbps)    *br_kbps     = br;
    if (frame_bytes) *frame_bytes = (144 * br * 1000) / s + pad;
    if (sr_idx)     *sr_idx      = 5 + sr2;    // row into the 8-row sfb tables
    return 1;
}

static int mp3_header_match(const uint8_t* a, const uint8_t* b) {
    return (a[1] & 0xFE) == (b[1] & 0xFE) &&
           (a[2] & 0x0C) == (b[2] & 0x0C) &&
           (a[3] & 0xC0) == (b[3] & 0xC0);
}

static int mp3_find_frame(const uint8_t* data, int size, int off, int* fb_out) {
    for (int i = off; i + 4 <= size; i++) {
        if (data[i] == 0xFF && (data[i + 1] & 0xE0) == 0xE0) {
            int fb;
            if (mp3_header(data + i, 0, 0, 0, &fb, 0) && fb > 0 && i + fb <= size) {
                if (i + fb + 4 > size || mp3_header_match(data + i, data + i + fb)) {
                    *fb_out = fb;
                    return i;
                }
            }
        }
    }
    return -1;
}

// ─── Granule info ─────────────────────────────────────────────────
typedef struct {
    uint16_t part2_3_length;
    uint16_t big_values;
    uint8_t  global_gain;
    uint8_t  scalefac_compress;
    uint8_t  table_select[3];
    uint8_t  region_count[3];
    uint8_t  preflag;
    uint8_t  scalefac_scale;
    uint8_t  count1_table;
    uint8_t  scfsi;
    uint8_t  block_type;          // 0 = long, 1 = start, 2 = short, 3 = stop
    uint8_t  mixed_block_flag;
    uint8_t  subblock_gain[3];
    uint8_t  n_long_sfb;
    uint8_t  n_short_sfb;
    const uint8_t* sfbtab;
} GI;

#define BT_NORM  0
#define BT_START 1
#define BT_SHORT 2
#define BT_STOP  3

// ─── Side information (MPEG1) ─────────────────────────────────────
// Returns main_data_begin (bytes) or -1 on a bad frame.
//
// scfsi layout mirrors the reference decoder exactly: the private bits
// and the per-channel scfsi fields are read as ONE combined field of
// 7+gr_count bits (5+4 mono, 3+8 stereo), then shifted through the
// granule loop.  Granule 0 of each channel always transmits its scale
// factors, so its scfsi slot is don't-care; the real values fall out at
// the granule-1 slots (stereo granule 1: ch0 gets bits 7..4, ch1 gets
// bits 3..0).
static int mp3_side_info(BR* b, GI* gr, int channels) {
    int gr_count = (channels == 1) ? 2 : 4;
    int main_data_begin = br_get(b, 9);
    unsigned scfsi = (unsigned)br_get(b, 7 + gr_count);

    for (int g = 0; g < gr_count; g++) {
        GI* gi = &gr[g];
        if (channels == 1) scfsi <<= 4;
        gi->part2_3_length = (uint16_t)br_get(b, 12);
        gi->big_values     = (uint16_t)br_get(b, 9);
        if (gi->big_values > 288) return -1;
        gi->global_gain       = (uint8_t)br_get(b, 8);
        gi->scalefac_compress = (uint8_t)br_get(b, 4);
        gi->sfbtab       = mp3_sfb_long[0];
        gi->n_long_sfb   = 22;
        gi->n_short_sfb  = 0;
        if (br_get(b, 1)) {                  // window_switching_flag
            gi->block_type = (uint8_t)br_get(b, 2);
            if (!gi->block_type) return -1;
            gi->mixed_block_flag = (uint8_t)br_get(b, 1);
            gi->region_count[0] = 7;
            gi->region_count[1] = 255;
            gi->region_count[2] = 255;
            if (gi->block_type == BT_SHORT) {
                // Short blocks never reuse scalefactors (scfsi only
                // applies to long blocks), so clear the reuse bits now —
                // otherwise the next granule may skip reading its scale
                // factors, misalign the bit position and decode garbage.
                scfsi &= 0x0F0F;
                if (!gi->mixed_block_flag) {
                    // Pure short block: region 0 spans the first 8 short
                    // scale-factor bands (ISO/IEC 11172-3 2.4.2.7).  The
                    // encoder does not transmit region counts here; the
                    // reference decoder hard-codes 8 (minimp3 L3_read_side_info),
                    // otherwise region 0 under-spans and the huffman walk
                    // misaligns after the 7th band.
                    gi->region_count[0] = 8;
                }
            }
            unsigned tables = (unsigned)br_get(b, 10);
            tables <<= 5;
            gi->subblock_gain[0] = (uint8_t)br_get(b, 3);
            gi->subblock_gain[1] = (uint8_t)br_get(b, 3);
            gi->subblock_gain[2] = (uint8_t)br_get(b, 3);
            gi->table_select[0] = (uint8_t)(tables >> 10);
            gi->table_select[1] = (uint8_t)((tables >> 5) & 31);
            gi->table_select[2] = (uint8_t)(tables & 31);
        } else {
            gi->block_type = 0;
            gi->mixed_block_flag = 0;
            unsigned tables = (unsigned)br_get(b, 15);
            gi->region_count[0] = (uint8_t)br_get(b, 4);
            gi->region_count[1] = (uint8_t)br_get(b, 3);
            gi->region_count[2] = 255;
            gi->table_select[0] = (uint8_t)(tables >> 10);
            gi->table_select[1] = (uint8_t)((tables >> 5) & 31);
            gi->table_select[2] = (uint8_t)(tables & 31);
        }
        gi->preflag        = (uint8_t)br_get(b, 1);
        gi->scalefac_scale = (uint8_t)br_get(b, 1);
        gi->count1_table   = (uint8_t)br_get(b, 1);
        gi->scfsi = (uint8_t)((scfsi >> 12) & 15);
        scfsi <<= 4;
    }
    return main_data_begin;
}

// ─── Scale factors (MPEG1) ────────────────────────────────────────
// Reads one granule's scalefactors into iscf_out (raw) and ist_out
// (same values on MPEG1; intensity positions come from channel 1).
// History (prev[]) is used when the scfsi reuse bit is set.
static int mp3_scalefactors(BR* b, GI* gi, uint8_t* iscf_out, uint8_t* ist_out,
                            const uint8_t* prev) {
    // The reference decodes scalefac_compress through the ISO
    // g_scfc_decode table (not the raw nibble halves):
    //   part = g_scfc_decode[compress]; s1 = part>>2; s2 = part&3
    static const uint8_t g_scfc_decode[16] = {
        0,1,2,3, 12,5,6,7, 9,10,11,13, 14,15,18,19 };
    int part = (gi->n_short_sfb ? 1 : 0) + (gi->n_long_sfb ? 0 : 1);
    const uint8_t* counts = mp3_scf_part[part];
    int p = g_scfc_decode[gi->scalefac_compress & 15];
    int s1 = p >> 2;
    int s2 = p & 3;
    int size[4] = { s1, s1, s2, s2 };
    int scfsi = gi->scfsi;
    int total = 0;
    for (int g = 0; g < 4 && counts[g]; g++, scfsi *= 2) {
        int cnt = counts[g];
        if (scfsi & 8) {                       // reuse granule 0's factors
            for (int k = 0; k < cnt; k++) {
                iscf_out[total + k] = prev[total + k];
                ist_out[total + k]  = prev[total + k];
            }
        } else {
            int bits = size[g];
            for (int k = 0; k < cnt; k++) {
                int s = bits ? br_get(b, bits) : 0;
                iscf_out[total + k] = (uint8_t)s;
                ist_out[total + k]  = (uint8_t)s;
            }
        }
        total += cnt;
    }
    for (int k = 0; k < 3; k++)
        iscf_out[total + k] = ist_out[total + k] = 0;
    return total;
}

// ─── Fixed-point helpers ──────────────────────────────────────────
// x^(4/3) in Q12 — port of the reference L3_pow_43 (145-entry table +
// 2-term Taylor correction).
static int32_t mp3_pow43_q12(int x) {
    int mult = 256;
    if (x < 129) return mp3_pow43[16 + x];
    if (x < 1024) { mult = 16; x <<= 3; }
    int sign = (2 * x) & 64;
    int num  = (x & 63) - sign;
    int den  = (x & ~63) + sign;
    if (den == 0) den = 1;
    int64_t frac = ((int64_t)num << 15) / den;          // Q15
    int64_t f1 = (frac * 43691) >> 15;                  // frac * 4/3
    int64_t f2 = (((frac * 7282) >> 15) * frac) >> 15;  // frac^2 * 2/9
    int64_t base = mp3_pow43[16 + ((x + sign) >> 6)];   // Q12
    int64_t res = base + ((base * (f1 + f2)) >> 15);
    return (int32_t)(res * mult);
}

// Trie-walk Huffman decode; returns symbol (x<<4)|y or quad 0..15, -1 on
// corrupt stream.
static int mp3_huff_sym(const Mp3HuffTable* t, BR* b) {
    int node = 0;
    for (int depth = 0; depth < 24; depth++) {
        uint16_t nxt = br_bit(b, b->pos++) ? t->c1[node] : t->c0[node];
        if (nxt == 0xFFFF) return -1;
        node = nxt;
        // sym is stored VALUE+1 so the sentinel 0 (internal node) can
        // never collide with the (15,15) = 0xFF leaf.
        if (t->sym[node] != 0) return t->sym[node] - 1;
    }
    return -1;
}

// Apply the per-sample dequantisation: xr = sign * pow43(q) * one.
// Output stays in Q12 so the small real values (~0.03 for typical
// frames) survive integer arithmetic; the synthesis filterbank's large
// window coefficients (up to 75038) rescale them to int16 range and
// the synth's final >>12 cancels this.
static inline int32_t mp3_xr(int q, int sg, int32_t one) {
    int64_t v = (int64_t)mp3_pow43_q12(q) * one;
    v >>= 16;                                  // Q12 (was Q28)
    if (sg < 0) v = -v;
    if (v > 0x3FFFFFFF) v = 0x3FFFFFFF;
    if (v < -0x3FFFFFFF) v = -0x3FFFFFFF;
    return (int32_t)v;
}

// ─── Huffman + dequant (one granule, one channel) ─────────────────
// Faithful port of the reference region walk: big values are decoded in
// pairs per scale-factor band; the count1 region continues mid-band with
// the leftover pairs (np = 1 - big_val_cnt) and stops at the granule's
// bit budget (limit).
static void mp3_huff_granule(BR* b, GI* gi, int32_t* xr, const int32_t* one, int limit) {
    int n = 0;
    int ireg = 0;
    int big_val_cnt = gi->big_values;
    const uint8_t* sfb = gi->sfbtab;
    int sfb_idx = 0;
    int one_cur = 0;

    while (big_val_cnt > 0) {
        int tab_num = gi->table_select[ireg];
        int sfb_cnt = gi->region_count[ireg++];
        const Mp3HuffTable* ht = &mp3_htabs[tab_num];
        int linbits = ht->linbits;
        do {
            // NOTE: big_val_cnt is reduced by the FULL band capacity np
            // (not the pairs actually decoded), exactly like the reference
            // decoder.  This drives big_val_cnt negative inside the last
            // partially-filled band, and count1's "1 - big_val_cnt" then
            // yields the correct number of leftover pairs in that band.
            int np = *sfb++ / 2;                    // pairs in this band
            int pairs = (big_val_cnt < np) ? big_val_cnt : np;
            one_cur = one[sfb_idx++];
            for (int p = 0; p < pairs; p++) {
                int sym = mp3_huff_sym(ht, b);
                if (sym < 0) goto done;
                int x = sym >> 4, y = sym & 15;
                for (int j = 0; j < 2; j++) {
                    int q = j ? y : x;
                    if (q == 15 && linbits) q += br_get(b, linbits);
                    int sg = 1;
                    if (q) sg = br_get(b, 1) ? -1 : 1;
                    xr[n++] = mp3_xr(q, sg, one_cur);
                }
            }
            big_val_cnt -= np;
        } while (big_val_cnt > 0 && --sfb_cnt >= 0);
    }

    // count1 region: quads of 4 values.  np starts as the leftover pairs
    // in the current band; a reload happens before the first pair AND
    // before the second pair of every quad (mirrors the reference).
    {
        const Mp3HuffTable* c1 = &mp3_htabs[32 + gi->count1_table];
        int np = 1 - big_val_cnt;                   // leftover pairs in band
        for (;;) {
            if (b->pos > limit) break;
            int sym = mp3_huff_sym(c1, b);
            if (sym < 0) break;
            if (b->pos > limit) break;
            // first pair (j=0,1): reload scalefactor before it
            if (!--np) {
                np = *sfb / 2;
                if (!np) break;
                sfb++;
                one_cur = one[sfb_idx++];
            }
            for (int j = 0; j < 2; j++) {
                int q = (sym >> (3 - j)) & 1;
                if (!q) { xr[n++] = 0; continue; }
                int sg = br_get(b, 1) ? -1 : 1;
                xr[n++] = mp3_xr(1, sg, one_cur);
            }
            // second pair (j=2,3): reload scalefactor before it
            if (!--np) {
                np = *sfb / 2;
                if (!np) break;
                sfb++;
                one_cur = one[sfb_idx++];
            }
            for (int j = 2; j < 4; j++) {
                int q = (sym >> (3 - j)) & 1;
                if (!q) { xr[n++] = 0; continue; }
                int sg = br_get(b, 1) ? -1 : 1;
                xr[n++] = mp3_xr(1, sg, one_cur);
            }
            if (n >= 576 || b->pos > limit) break;
        }
    }
done:
    while (n < 576) xr[n++] = 0;
    // ALWAYS snap the bit position to this granule's budget, exactly like
    // the reference (bs->pos = layer3gr_limit).  When the count1 region
    // fills all 576 values before the bit budget runs out, the encoder's
    // padding must be skipped or every subsequent granule misaligns.
    b->pos = limit;
}

// ─── Reorder (short blocks) ───────────────────────────────────────
static void mp3_reorder(int32_t* grbuf, int32_t* scratch, const uint8_t* sfb) {
    int32_t* src = grbuf;
    int32_t* dst = scratch;
    for (;;) {
        int len = *sfb;
        if (!len) break;
        for (int i = 0; i < len; i++, src++) {
            *dst++ = src[0 * len];
            *dst++ = src[1 * len];
            *dst++ = src[2 * len];
        }
        sfb += 3;
        src += 2 * len;
    }
    int n = (int)(dst - scratch);
    for (int i = 0; i < n; i++) grbuf[i] = scratch[i];
}

// ─── Stereo processing ────────────────────────────────────────────
static void mp3_midside(int32_t* left, int n) {
    int32_t* right = left + 576;
    for (int i = 0; i < n; i++) {
        int64_t a = left[i];
        int64_t b = right[i];
        // Mid/side are already halved by the encoder; the MS scale is
        // carried by the one[] dequantizer exponent (-2 in gain_exp),
        // exactly like the reference decoder.  Adding a >>1 here would
        // double-attenuate MS frames to half amplitude.
        left[i]  = (int32_t)(a + b);
        right[i] = (int32_t)(a - b);
    }
}

static void mp3_intensity_band(int32_t* left, int n, int32_t kl, int32_t kr) {
    int32_t* right = left + 576;
    for (int i = 0; i < n; i++) {
        int64_t l = (int64_t)left[i] * kl;
        int64_t r = (int64_t)left[i] * kr;
        left[i]  = (int32_t)(l >> 15);
        right[i] = (int32_t)(r >> 15);
    }
}

static void mp3_stereo_top_band(const int32_t* right, const uint8_t* sfb,
                                int nbands, int max_band[3]) {
    max_band[0] = max_band[1] = max_band[2] = -1;
    for (int i = 0; i < nbands; i++) {
        for (int k = 0; k < sfb[i]; k += 2) {
            if (right[k] != 0 || right[k + 1] != 0) {
                max_band[i % 3] = i;
                break;
            }
        }
        right += sfb[i];
    }
}

static void mp3_stereo_process(int32_t* left, const uint8_t* ist_pos,
                               const uint8_t* sfb, int max_band[3], int ms) {
    for (int i = 0; sfb[i]; i++) {
        int ipos = ist_pos[i];
        if (i > max_band[i % 3] && ipos < 7) {
            int32_t kl = mp3_pan[2 * ipos];
            int32_t kr = mp3_pan[2 * ipos + 1];
            if (ms) { kl <<= 1; kr <<= 1; }        // sqrt(2) in Q15
            mp3_intensity_band(left, sfb[i], kl, kr);
        } else if (ms) {
            mp3_midside(left, sfb[i]);
        }
        left += sfb[i];
    }
}

static void mp3_intensity(int32_t* grbuf, uint8_t* ist_pos, const GI* gi, int ms) {
    int max_band[3];
    int n_sfb = gi->n_long_sfb + gi->n_short_sfb;
    int max_blocks = gi->n_short_sfb ? 3 : 1;
    mp3_stereo_top_band(grbuf + 576, gi->sfbtab, n_sfb, max_band);
    if (gi->n_long_sfb) {
        int m = max_band[0];
        if (max_band[1] > m) m = max_band[1];
        if (max_band[2] > m) m = max_band[2];
        max_band[0] = max_band[1] = max_band[2] = m;
    }
    for (int i = 0; i < max_blocks; i++) {
        int itop = n_sfb - max_blocks + i;
        int prev = itop - max_blocks;
        ist_pos[itop] = (max_band[i] >= prev) ? 3 : ist_pos[prev];
    }
    mp3_stereo_process(grbuf, ist_pos, gi->sfbtab, max_band, ms);
}

// ─── Antialias ────────────────────────────────────────────────────
static void mp3_antialias(int32_t* grbuf, int nbands) {
    for (; nbands > 0; nbands--, grbuf += 18) {
        for (int i = 0; i < 8; i++) {
            int64_t u = grbuf[18 + i];
            int64_t d = grbuf[17 - i];
            int64_t c0 = mp3_aa[0 + i];
            int64_t c1 = mp3_aa[8 + i];
            grbuf[18 + i] = (int32_t)((u * c0 - d * c1) >> 15);
            grbuf[17 - i] = (int32_t)((u * c1 + d * c0) >> 15);
        }
    }
}

// ─── IMDCT ────────────────────────────────────────────────────────
static void mp3_dct3_9(int64_t* y) {
    int64_t s0, s1, s2, s3, s4, s5, s6, s7, s8, t0, t2, t4;
    s0 = y[0]; s2 = y[2]; s4 = y[4]; s6 = y[6]; s8 = y[8];
    t0 = s0 + (s6 >> 1);
    s0 -= s6;
    t4 = ((s4 + s2) * 30789) >> 15;     // 0.93969262
    t2 = ((s8 + s2) * 25100) >> 15;     // 0.76604444
    s6 = ((s4 - s8) * 5692) >> 15;      // 0.17364818
    s4 += s8 - s2;
    s2 = s0 - (s4 >> 1);
    y[4] = s4 + s0;
    s8 = t0 - t2 + s6;
    s0 = t0 - t4 + t2;
    s4 = t0 + t4 - s6;
    s1 = y[1]; s3 = y[3]; s5 = y[5]; s7 = y[7];
    s3 = (s3 * 28378) >> 15;            // 0.86602540
    t0 = ((s5 + s1) * 32271) >> 15;     // 0.98480775
    t4 = ((s5 - s7) * 11209) >> 15;     // 0.34202014
    t2 = ((s1 + s7) * 21060) >> 15;     // 0.64278761
    s1 = ((s1 - s5 - s7) * 28378) >> 15;
    s5 = t0 - s3 - t2;
    s7 = t4 - s3 - t0;
    s3 = t4 + s3 - t2;
    y[0] = s4 - s7;
    y[1] = s2 + s1;
    y[2] = s0 - s3;
    y[3] = s8 + s5;
    y[5] = s8 - s5;
    y[6] = s0 + s3;
    y[7] = s2 - s1;
    y[8] = s4 + s7;
}

static void mp3_imdct36(int32_t* grbuf, int32_t* overlap, const int16_t* win, int nbands) {
    for (int j = 0; j < nbands; j++, grbuf += 18, overlap += 9) {
        int64_t co[9], si[9];
        co[0] = -grbuf[0];
        si[0] = grbuf[17];
        for (int i = 0; i < 4; i++) {
            si[8 - 2 * i] =  grbuf[4 * i + 1] - grbuf[4 * i + 2];
            co[1 + 2 * i] =  grbuf[4 * i + 1] + grbuf[4 * i + 2];
            si[7 - 2 * i] =  grbuf[4 * i + 4] - grbuf[4 * i + 3];
            co[2 + 2 * i] = -(grbuf[4 * i + 3] + grbuf[4 * i + 4]);
        }
        mp3_dct3_9(co);
        mp3_dct3_9(si);
        si[1] = -si[1]; si[3] = -si[3]; si[5] = -si[5]; si[7] = -si[7];
        for (int i = 0; i < 9; i++) {
            int64_t ovl = overlap[i];
            int64_t sum = ((co[i] * mp3_twid9[9 + i]) >> 15) + ((si[i] * mp3_twid9[i]) >> 15);
            overlap[i]  = ((co[i] * mp3_twid9[i]) >> 15) - ((si[i] * mp3_twid9[9 + i]) >> 15);
            grbuf[i]      = (int32_t)(((ovl * win[i]) >> 15) - ((sum * win[9 + i]) >> 15));
            grbuf[17 - i] = (int32_t)(((ovl * win[9 + i]) >> 15) + ((sum * win[i]) >> 15));
        }
    }
}

static void mp3_idct3(int64_t x0, int64_t x1, int64_t x2, int64_t* dst) {
    int64_t m1 = (x1 * 28378) >> 15;        // 0.86602540
    int64_t a1 = x0 - (x2 >> 1);
    dst[1] = x0 + x2;
    dst[0] = a1 + m1;
    dst[2] = a1 - m1;
}

static void mp3_imdct12(const int32_t* x, int32_t* dst, int32_t* overlap) {
    int64_t co[3], si[3];
    mp3_idct3(-x[0], x[6] + x[3], x[12] + x[9], co);
    mp3_idct3(x[15], x[12] - x[9], x[6] - x[3], si);
    si[1] = -si[1];
    for (int i = 0; i < 3; i++) {
        int64_t ovl = overlap[i];
        int64_t sum = ((co[i] * mp3_twid3[3 + i]) >> 15) + ((si[i] * mp3_twid3[i]) >> 15);
        overlap[i]  = ((co[i] * mp3_twid3[i]) >> 15) - ((si[i] * mp3_twid3[3 + i]) >> 15);
        dst[i]     = (int32_t)(((ovl * mp3_twid3[2 - i]) >> 15) - ((sum * mp3_twid3[5 - i]) >> 15));
        dst[5 - i] = (int32_t)(((ovl * mp3_twid3[5 - i]) >> 15) + ((sum * mp3_twid3[2 - i]) >> 15));
    }
}

static void mp3_imdct_short(int32_t* grbuf, int32_t* overlap, int nbands) {
    for (; nbands > 0; nbands--, overlap += 9, grbuf += 18) {
        int32_t tmp[18];
        for (int i = 0; i < 18; i++) tmp[i] = grbuf[i];
        for (int i = 0; i < 6; i++) grbuf[i] = overlap[i];
        mp3_imdct12(tmp, grbuf + 6, overlap + 6);
        mp3_imdct12(tmp + 1, grbuf + 12, overlap + 6);
        mp3_imdct12(tmp + 2, overlap, overlap + 6);
    }
}

static void mp3_change_sign(int32_t* grbuf) {
    for (int b = 0, o = 18; b < 32; b += 2, o += 36)
        for (int i = 1; i < 18; i += 2)
            grbuf[o + i] = -grbuf[o + i];
}

static void mp3_imdct_gr(int32_t* grbuf, int32_t* overlap, unsigned block_type,
                         unsigned n_long_bands) {
    const int16_t* w0 = mp3_mdct_win;
    const int16_t* w1 = mp3_mdct_win + 18;
    if (n_long_bands) {
        mp3_imdct36(grbuf, overlap, w0, (int)n_long_bands);
        grbuf   += 18 * n_long_bands;
        overlap += 9 * n_long_bands;
    }
    if (block_type == BT_SHORT)
        mp3_imdct_short(grbuf, overlap, 32 - (int)n_long_bands);
    else
        mp3_imdct36(grbuf, overlap, (block_type == BT_STOP) ? w1 : w0,
                    32 - (int)n_long_bands);
}

// ─── Synthesis filterbank ─────────────────────────────────────────
static void mp3_dct_ii(int32_t* grbuf, int n) {
    for (int k = 0; k < n; k++) {
        int64_t t[4][8];
        int32_t* y = grbuf + k;
        for (int i = 0; i < 8; i++) {
            int64_t x0 = y[i * 18];
            int64_t x1 = y[(15 - i) * 18];
            int64_t x2 = y[(16 + i) * 18];
            int64_t x3 = y[(31 - i) * 18];
            int64_t t0 = x0 + x3;
            int64_t t1 = x1 + x2;
            int64_t t2 = ((x1 - x2) * mp3_sec[3 * i + 0]) >> 11;
            int64_t t3 = ((x0 - x3) * mp3_sec[3 * i + 1]) >> 11;
            t[0][i] = t0 + t1;
            t[1][i] = ((t0 - t1) * mp3_sec[3 * i + 2]) >> 11;
            t[2][i] = t3 + t2;
            t[3][i] = ((t3 - t2) * mp3_sec[3 * i + 2]) >> 11;
        }
        for (int i = 0; i < 4; i++) {
            int64_t* x = t[i];
            int64_t x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
            int64_t x4 = x[4], x5 = x[5], x6 = x[6], x7 = x[7], xt;
            xt = x0 - x7; x0 += x7;
            x7 = x1 - x6; x1 += x6;
            x6 = x2 - x5; x2 += x5;
            x5 = x3 - x4; x3 += x4;
            x4 = x0 - x3; x0 += x3;
            x3 = x1 - x2; x1 += x2;
            x[0] = x0 + x1;
            x[4] = ((x0 - x1) * 23170) >> 15;               // 0.70710677
            x5 += x6;
            x6 = ((x6 + x7) * 23170) >> 15;
            x7 += xt;
            x3 = ((x3 + x4) * 23170) >> 15;
            x5 -= (x7 * 6520) >> 15;                        // 0.198912367
            x7 += (x5 * 12540) >> 15;                       // 0.382683432
            x5 -= (x7 * 6520) >> 15;
            x0 = xt - x6; xt += x6;
            x[1] = ((xt + x7) * 16705) >> 15;               // 0.50979561
            x[2] = ((x4 + x3) * 17734) >> 15;               // 0.54119611
            x[3] = ((x0 - x5) * 19705) >> 15;               // 0.60134488
            x[5] = ((x0 + x5) * 29490) >> 15;               // 0.89997619
            x[6] = ((x4 - x3) * 42813) >> 15;               // 1.30656302
            x[7] = ((xt - x7) * 83984) >> 15;               // 2.56291556
        }
        for (int i = 0; i < 7; i++, y += 4 * 18) {
            y[0 * 18] = (int32_t)t[0][i];
            y[1 * 18] = (int32_t)(t[2][i] + t[3][i] + t[3][i + 1]);
            y[2 * 18] = (int32_t)(t[1][i] + t[1][i + 1]);
            y[3 * 18] = (int32_t)(t[2][i + 1] + t[3][i] + t[3][i + 1]);
        }
        y[0 * 18] = (int32_t)t[0][7];
        y[1 * 18] = (int32_t)(t[2][7] + t[3][7]);
        y[2 * 18] = (int32_t)t[1][7];
        y[3 * 18] = (int32_t)t[3][7];
    }
}

static inline int16_t mp3_clamp16(int64_t v) {
    if (v >= 32767) return 32767;
    if (v <= -32767) return -32768;
    return (int16_t)v;
}

static void mp3_synth_pair(int16_t* pcm, int nch, const int32_t* z) {
    int64_t a;
    a  = (int64_t)(z[14 * 64] - z[0]) * 29;
    a += (int64_t)(z[1 * 64] + z[13 * 64]) * 213;
    a += (int64_t)(z[12 * 64] - z[2 * 64]) * 459;
    a += (int64_t)(z[3 * 64] + z[11 * 64]) * 2037;
    a += (int64_t)(z[10 * 64] - z[4 * 64]) * 5153;
    a += (int64_t)(z[5 * 64] + z[9 * 64]) * 6574;
    a += (int64_t)(z[8 * 64] - z[6 * 64]) * 37489;
    a += (int64_t)z[7 * 64] * 75038;
    pcm[0] = mp3_clamp16(a >> 12);              // z is Q12

    z += 2;
    a  = (int64_t)z[14 * 64] * 104;
    a += (int64_t)z[12 * 64] * 1567;
    a += (int64_t)z[10 * 64] * 9727;
    a += (int64_t)z[8 * 64] * 64019;
    a += (int64_t)z[6 * 64] * -9975;
    a += (int64_t)z[4 * 64] * -45;
    a += (int64_t)z[2 * 64] * 146;
    a += (int64_t)z[0 * 64] * -5;
    pcm[16 * nch] = mp3_clamp16(a >> 12);       // z is Q12
}

// Port of the reference mp3d_synth (scalar branch).  xl = 2ch x 576 time
// samples (columns processed in pairs), produces 64 PCM frames per call.
static void mp3_synth(int32_t* xl, int16_t* dstl, int nch, int32_t* lins) {
    int32_t* xr = xl + 576 * (nch - 1);
    int16_t* dstr = dstl + (nch - 1);
    int32_t* zlin = lins + 15 * 64;
    const int32_t* w = mp3_win;

    zlin[4 * 15]     = xl[18 * 16];
    zlin[4 * 15 + 1] = xr[18 * 16];
    zlin[4 * 15 + 2] = xl[0];
    zlin[4 * 15 + 3] = xr[0];
    zlin[4 * 31]     = xl[1 + 18 * 16];
    zlin[4 * 31 + 1] = xr[1 + 18 * 16];
    zlin[4 * 31 + 2] = xl[1];
    zlin[4 * 31 + 3] = xr[1];

    mp3_synth_pair(dstr, nch, lins + 4 * 15 + 1);
    mp3_synth_pair(dstr + 32 * nch, nch, lins + 4 * 15 + 64 + 1);
    mp3_synth_pair(dstl, nch, lins + 4 * 15);
    mp3_synth_pair(dstl + 32 * nch, nch, lins + 4 * 15 + 64);

    for (int i = 14; i >= 0; i--) {
        int64_t a[4], b[4];
        zlin[4 * i]           = xl[18 * (31 - i)];
        zlin[4 * i + 1]       = xr[18 * (31 - i)];
        zlin[4 * i + 2]       = xl[1 + 18 * (31 - i)];
        zlin[4 * i + 3]       = xr[1 + 18 * (31 - i)];
        zlin[4 * (i + 16)]    = xl[1 + 18 * (1 + i)];
        zlin[4 * (i + 16) + 1] = xr[1 + 18 * (1 + i)];
        zlin[4 * (i - 16) + 2] = xl[18 * (1 + i)];
        zlin[4 * (i - 16) + 3] = xr[18 * (1 + i)];

        for (int k = 0; k < 8; k++) {
            int32_t w0 = *w++;
            int32_t w1 = *w++;
            int32_t* vz = &zlin[4 * i - k * 64];
            int32_t* vy = &zlin[4 * i - (15 - k) * 64];
            if (k == 0) {                       // S0: assign
                for (int j = 0; j < 4; j++) {
                    b[j] = (int64_t)vz[j] * w1 + (int64_t)vy[j] * w0;
                    a[j] = (int64_t)vz[j] * w0 - (int64_t)vy[j] * w1;
                }
            } else if (k & 1) {                 // S2
                for (int j = 0; j < 4; j++) {
                    b[j] += (int64_t)vz[j] * w1 + (int64_t)vy[j] * w0;
                    a[j] += (int64_t)vy[j] * w1 - (int64_t)vz[j] * w0;
                }
            } else {                            // S1
                for (int j = 0; j < 4; j++) {
                    b[j] += (int64_t)vz[j] * w1 + (int64_t)vy[j] * w0;
                    a[j] += (int64_t)vz[j] * w0 - (int64_t)vy[j] * w1;
                }
            }
        }
        dstr[(15 - i) * nch] = mp3_clamp16(a[1] >> 12);
        dstr[(17 + i) * nch] = mp3_clamp16(b[1] >> 12);
        dstl[(15 - i) * nch] = mp3_clamp16(a[0] >> 12);
        dstl[(17 + i) * nch] = mp3_clamp16(b[0] >> 12);
        dstr[(47 - i) * nch] = mp3_clamp16(a[3] >> 12);
        dstr[(49 + i) * nch] = mp3_clamp16(b[3] >> 12);
        dstl[(47 - i) * nch] = mp3_clamp16(a[2] >> 12);
        dstl[(49 + i) * nch] = mp3_clamp16(b[2] >> 12);
    }
}

static void mp3_synth_granule(Mp3Dec* d, int nch, int16_t* pcm) {
    for (int c = 0; c < nch; c++)
        mp3_dct_ii(d->grbuf + 576 * c, 18);

    for (int i = 0; i < 15 * 64; i++) d->syn[i] = d->qmf[0][i];

    for (int i = 0; i < 18; i += 2)
        mp3_synth(d->grbuf + i, pcm + 32 * nch * i, nch, d->syn + i * 64);

    if (nch == 1) {
        for (int i = 0; i < 15 * 64; i += 2)
            d->qmf[0][i] = d->syn[18 * 64 + i];
    } else {
        for (int i = 0; i < 15 * 64; i++) d->qmf[0][i] = d->syn[18 * 64 + i];
    }
}

// ─── Granule decode ───────────────────────────────────────────────
static void mp3_granule(Mp3Dec* d, BR* b, GI* gi, int ch, int ms) {
    // granule bit budget (scalefactors + huffman together).  Computed
    // BEFORE mp3_scalefactors consumes bits — the reference does
    // layer3gr_limit = bs.pos + part_23_length at granule start, so
    // part_23_length covers the scalefactor bits too.  Computing it
    // after would count the scalefactor bits twice and steal budget
    // from every subsequent granule (frames decode as silence).
    int limit = b->pos + gi->part2_3_length;
    if (limit > b->len * 8) limit = b->len * 8;

    int32_t one[44];
    uint8_t iscf[44];
    uint8_t ist[44];
    int nsfb = mp3_scalefactors(b, gi, iscf, ist, d->iscf_hist[ch]);

    // save raw scalefactors for scfsi reuse + intensity positions
    for (int i = 0; i < nsfb + 3 && i < 40; i++) d->iscf_hist[ch][i] = iscf[i];

    // apply subblock_gain (short) or preflag (long)
    int sh = gi->scalefac_scale + 1;
    if (gi->n_short_sfb) {
        int sg = 3 - sh;
        for (int i = 0; i < gi->n_short_sfb; i += 3) {
            iscf[gi->n_long_sfb + i + 0] += (uint8_t)(gi->subblock_gain[0] << sg);
            iscf[gi->n_long_sfb + i + 1] += (uint8_t)(gi->subblock_gain[1] << sg);
            iscf[gi->n_long_sfb + i + 2] += (uint8_t)(gi->subblock_gain[2] << sg);
        }
    } else if (gi->preflag) {
        for (int i = 0; i < 10; i++) iscf[11 + i] += mp3_pretab[i];
    }

    // per-band scale 2^(E/4).  The exponent base mirrors the reference
    // decoder: gain_exp = global_gain - 214 - 2*ms (214 = 210 + 4, the
    // constant folded from BITS_DEQUANTIZER_OUT=-1), then
    // scf = 2^((gain_exp - (iscf<<sh))/4).  pow2q is 2^((k-512)/4) in
    // Q16 and pow43 is Q12, so the product is Q28; mp3_xr() shifts
    // it down to Q12 and the synthesis filterbank's final >>12 cancels
    // the scale, leaving xr at the reference float scale.
    int gg = gi->global_gain;
    int nsfb_all = gi->n_long_sfb + gi->n_short_sfb;
    for (int i = 0; i < nsfb_all; i++) {
        int E = gg - 214 - (ms ? 2 : 0) - ((int)iscf[i] << sh);
        int k = E + 512;
        if (k < 0) k = 0;
        if (k > 1023) k = 1023;
        one[i] = mp3_pow2q[k];
    }
    for (int i = nsfb_all; i < 44; i++) one[i] = one[nsfb_all - 1];

    mp3_huff_granule(b, gi, d->grbuf + 576 * ch, one, limit);
    // The reference decoder forces the bitstream position to the exact
    // granule boundary after the huffman walk (bs->pos = layer3gr_limit).
    // The huffman data almost never fills part2_3_length exactly, so the
    // leftover bits are padding and MUST be skipped — otherwise the next
    // granule starts mid-padding and decodes as near-silence.
    b->pos = limit;

    // intensity positions (channel 1) need the raw ist values
    if (ch == 1) {
        for (int i = 0; i < 40; i++) d->ist_hist[i] = ist[i];
    }
}

// ─── Frame decode ─────────────────────────────────────────────────
int mp3_header_frame_bytes(const uint8_t* data, int size, int off) {
    if (off < 0 || off + 4 > size) return 0;
    int fb;
    if (!mp3_header(data + off, 0, 0, 0, &fb, 0)) return 0;
    return fb;
}

int mp3_init(Mp3Dec* d, const uint8_t* data, int size) {
    int off = 0;
    if (size >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        int sz = (data[6] << 21) | (data[7] << 14) | (data[8] << 7) | data[9];
        off = 10 + sz;
        if (data[5] & 0x10) off += 10;
    }
    if (off >= size) return -1;

    int fb = 0;
    int f = mp3_find_frame(data, size, off, &fb);
    if (f < 0 || fb <= 0) return -1;

    d->data = data;
    d->size = size;
    d->frame_off = f;
    d->frame_bytes = fb;
    d->reserv_len = 0;
    d->pcm_samples = 0;
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 40; i++) d->iscf_hist[c][i] = 0;
        for (int i = 0; i < 288; i++) d->overlap[c][i] = 0;
        for (int i = 0; i < 960; i++) d->qmf[c][i] = 0;
    }
    for (int i = 0; i < 40; i++) d->ist_hist[i] = 0;
    mp3_header(d->data + d->frame_off, &d->channels, &d->samplerate,
               &d->bitrate, 0, &d->sr_idx);
    int mode = (d->data[d->frame_off + 3] >> 6) & 3;
    int mode_ext = (d->data[d->frame_off + 3] >> 4) & 3;
    d->ms_stereo = (mode == 1) && (mode_ext & 2);
    d->i_stereo  = (mode != 3) && (mode_ext & 1);
    for (int i = 0; i < 4; i++) d->last_hdr[i] = d->data[d->frame_off + i];
    return f;
}

int mp3_decode_frame(Mp3Dec* d) {
    int off = d->frame_off;
    if (off < 0 || off + 4 > d->size) return 0;

    int fb = 0;
    if (d->data[off] == 0xFF && (d->data[off + 1] & 0xE0) == 0xE0)
        mp3_header(d->data + off, 0, 0, 0, &fb, 0);
    if (fb <= 0) {
        int f = mp3_find_frame(d->data, d->size, off, &fb);
        if (f < 0 || fb <= 0) return 0;
        off = f;
        d->frame_off = f;
    }
    if (off + fb > d->size) return 0;
    d->frame_bytes = fb;

    const uint8_t* h = d->data + off;
    int channels, sr, br, sr_idx;
    mp3_header(h, &channels, &sr, &br, 0, &sr_idx);
    d->channels = channels;
    d->samplerate = sr;
    d->bitrate = br;
    d->sr_idx = sr_idx;
    int mode = (h[3] >> 6) & 3;
    int mode_ext = (h[3] >> 4) & 3;
    d->ms_stereo = (mode == 1) && (mode_ext & 2);
    d->i_stereo  = (mode != 3) && (mode_ext & 1);

    // XING / Info tag frames carry no audio — skip them.
    int si_bytes = (channels == 1) ? 21 : 36;   // 4 + side-info bytes
    if (fb > si_bytes + 4 &&
        h[si_bytes] == 'X' && h[si_bytes + 1] == 'i' &&
        h[si_bytes + 2] == 'n' && h[si_bytes + 3] == 'g') {
        d->frame_off = off + fb;
        return 0;
    }
    if (fb > si_bytes + 4 &&
        h[si_bytes] == 'I' && h[si_bytes + 1] == 'n' &&
        h[si_bytes + 2] == 'f' && h[si_bytes + 3] == 'o') {
        d->frame_off = off + fb;
        return 0;
    }

    int crc = (h[1] & 1) ? 0 : 16;
    BR frame_br;
    frame_br.buf = h + 4;
    frame_br.len = fb - 4;
    frame_br.pos = 0;
    if (crc) br_get(&frame_br, 16);

    GI gr[4];
    int main_data_begin = mp3_side_info(&frame_br, gr, channels);
    if (main_data_begin < 0 || frame_br.pos > (fb - 4) * 8) {
        d->frame_off = off + fb;
        return 0;
    }

    int payload_off = (frame_br.pos + 7) >> 3;
    int payload_bytes = (fb - 4) - payload_off;
    if (payload_bytes < 0) payload_bytes = 0;
    int bytes_have = main_data_begin < d->reserv_len ? main_data_begin : d->reserv_len;
    int rstart = (d->reserv_len > main_data_begin) ? d->reserv_len - main_data_begin : 0;
    int md = 0;
    for (int i = 0; i < bytes_have; i++)
        d->maindata[md++] = d->reserv[rstart + i];
    for (int i = 0; i < payload_bytes && 4 + payload_off + i < fb; i++)
        d->maindata[md++] = h[4 + payload_off + i];

    BR b;
    b.buf = d->maindata;
    b.len = md;
    b.pos = 0;

    int ngr = 2;
    int16_t* pcm = d->pcm_flat;
    int ok = 1;
    for (int g = 0; g < ngr; g++) {
        int gbase = g * channels;
        for (int ch = 0; ch < channels; ch++) {
            GI* gi = &gr[gbase + ch];
            if (gi->block_type == BT_SHORT) {
                gi->n_short_sfb = gi->mixed_block_flag ? 30 : 39;
                gi->n_long_sfb  = gi->mixed_block_flag ? 8 : 0;
            } else {
                gi->n_short_sfb = 0;
                gi->n_long_sfb  = 22;
            }
            gi->sfbtab = (gi->n_short_sfb == 0)
                ? mp3_sfb_long[sr_idx - 5]
                : (gi->mixed_block_flag ? mp3_sfb_mixed[sr_idx - 5]
                                        : mp3_sfb_short[sr_idx - 5]);
            for (int i = 0; i < 576; i++) d->grbuf[576 * ch + i] = 0;
            mp3_granule(d, &b, gi, ch, d->ms_stereo);
            if (b.pos < 0) ok = 0;
        }
        if (d->i_stereo) {
            mp3_intensity(d->grbuf, d->ist_hist, &gr[gbase], d->ms_stereo);
        } else if (d->ms_stereo) {
            mp3_midside(d->grbuf, 576);
        }
        for (int ch = 0; ch < channels; ch++) {
            GI* gi = &gr[gbase + ch];
            int n_long_bands = gi->mixed_block_flag ? 2 : 0;
            int aa_bands = 31;
            if (gi->n_short_sfb) {
                aa_bands = n_long_bands - 1;
                mp3_reorder(d->grbuf + 576 * ch + n_long_bands * 18,
                            d->reorder_scratch, gi->sfbtab + gi->n_long_sfb);
            }
            mp3_antialias(d->grbuf + 576 * ch, aa_bands);
            mp3_imdct_gr(d->grbuf + 576 * ch, d->overlap[ch],
                         gi->block_type, (unsigned)n_long_bands);
            mp3_change_sign(d->grbuf + 576 * ch);
        }
        mp3_synth_granule(d, channels, pcm);
        pcm += 576 * channels;
    }

    // save the bit reservoir (main data after the last granule)
    int end_byte = (b.pos + 7) >> 3;
    int remains = md - end_byte;
    if (remains > MP3_RESERV_BYTES) {
        end_byte += remains - MP3_RESERV_BYTES;
        remains = MP3_RESERV_BYTES;
    }
    if (remains > 0 && end_byte >= 0 && end_byte + remains <= md) {
        for (int i = 0; i < remains; i++) d->reserv[i] = d->maindata[end_byte + i];
        d->reserv_len = remains;
    } else {
        d->reserv_len = 0;
    }

    // de-interleave into per-channel output
    if (channels == 1) {
        for (int i = 0; i < MP3_FRAME_SAMPLES; i++) {
            d->pcm[0][i] = d->pcm_flat[i];
            d->pcm[1][i] = 0;
        }
    } else {
        for (int i = 0; i < MP3_FRAME_SAMPLES; i++) {
            d->pcm[0][i] = d->pcm_flat[2 * i];
            d->pcm[1][i] = d->pcm_flat[2 * i + 1];
        }
    }

    d->frame_off = off + fb;
    d->pcm_samples = MP3_FRAME_SAMPLES;
    return ok ? MP3_FRAME_SAMPLES : 0;
}
