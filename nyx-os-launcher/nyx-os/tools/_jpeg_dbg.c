// tools/_jpeg_dbg.c — debug harness: decode a JPEG and print the
// dequantized coefficients of specific MCU blocks, so the C decoder
// can be compared bit-for-bit against the Python reference decoder.
// Usage: _jpeg_dbg <in.jpg> <mcx> <mcy>
#include <jpegdec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── copy of the decoder internals with printf instrumentation ──
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

#define JPEG_HT_NODES 512
typedef struct { int16_t kids[2]; int16_t sym; } JHtNode;
typedef struct { JHtNode nodes[JPEG_HT_NODES]; int count; } JHtTree;

static int jht_new_node(JHtTree* t) {
    if (t->count >= JPEG_HT_NODES) return -1;
    int idx = t->count++;
    t->nodes[idx].kids[0] = -1; t->nodes[idx].kids[1] = -1; t->nodes[idx].sym = -1;
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

// ── verbatim copy of cpu/jpegdec.c idct_2d ──
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
static void idct_2d(const int* coeffs, uint8_t* output) {
    static const int zig[64] = {
         0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
    };
    int blk[8][8];
    for (int i = 0; i < 64; i++) blk[i/8][i%8] = coeffs[zig[i]];
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
            int val = (int)(sum / 262144);
            val += 128;
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            output[y * 8 + x] = (uint8_t)val;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc(sz); fread(d, 1, sz, f); fclose(f);
    int datalen = (int)sz;
    int want_mx = atoi(argv[2]), want_my = atoi(argv[3]);

    int pos = 2;
    int width = 0, height = 0, num_components = 0;
    int comp_id[4] = {0}, comp_h[4] = {0}, comp_v[4] = {0};
    uint8_t quant_tables[4][64];
    int quant_present[4] = {0};
    JHtTree ht_tables[6];
    int ht_roots[6] = {0}, ht_present[6] = {0};
    int comp_dc_tbl[4] = {0}, comp_ac_tbl[4] = {0};
    int max_h = 0, max_v = 0, scan_start = 0;

    while (pos + 1 < datalen) {
        if (d[pos] != 0xFF) { pos++; continue; }
        while (pos < datalen && d[pos] == 0xFF) pos++;
        if (pos >= datalen) break;
        uint8_t marker = d[pos++];
        if (marker == 0xD9) break;
        if (marker == 0x00 || marker == 0xD8) continue;
        if (pos + 1 >= datalen) break;
        int seg_len = ((int)d[pos] << 8) | (int)d[pos + 1];
        if (seg_len < 2) continue;
        int seg_end = pos + seg_len;
        if (seg_end > datalen) break;

        if (marker == 0xDB) {
            int qpos = pos + 2;
            while (qpos + 65 <= seg_end) {
                int info = d[qpos++];
                int tid = info & 0x0F;
                if (tid > 3) break;
                for (int i = 0; i < 64; i++) quant_tables[tid][i] = d[qpos++];
                quant_present[tid] = 1;
            }
        } else if (marker == 0xC0) {
            if (pos + 8 >= datalen || d[pos+2] != 8) return 1;
            height = ((int)d[pos+3]<<8)|d[pos+4];
            width  = ((int)d[pos+5]<<8)|d[pos+6];
            num_components = d[pos+7];
            if ((num_components != 3 && num_components != 4) || width<=0 || height>4096) return 1;
            for (int i = 0; i < num_components; i++) {
                int ci = pos + 8 + i*3;
                comp_id[i] = d[ci];
                comp_h[i] = (d[ci+1]>>4)&0x0F;
                comp_v[i] = d[ci+1]&0x0F;
                comp_dc_tbl[i] = d[ci+2];
                if (comp_h[i] > max_h) max_h = comp_h[i];
                if (comp_v[i] > max_v) max_v = comp_v[i];
            }
        } else if (marker == 0xC4) {
            int hp = pos + 2;
            while (hp + 17 <= seg_end) {
                int info = d[hp++];
                int cls = (info>>4)&0x0F, tid = info&0x0F;
                if (tid > 3) break;
                int idx = tid + (cls ? 2 : 0);
                uint8_t bits[16]; int ns = 0;
                for (int i = 0; i < 16; i++) { bits[i]=d[hp++]; ns+=bits[i]; }
                ht_roots[idx] = jht_build(&ht_tables[idx], bits, d+hp, ns);
                ht_present[idx] = 1;
                hp += ns;
            }
        } else if (marker == 0xDA) {
            int nsc = d[pos+2];
            if (nsc < 1 || nsc > 4) break;
            for (int i = 0; i < nsc; i++) {
                int si = pos + 3 + i*2;
                int sid = d[si];
                for (int j=0;j<num_components;j++)
                    if (comp_id[j]==sid) {
                        comp_dc_tbl[j]=(d[si+1]>>4)&0x0F;
                        comp_ac_tbl[j]=d[si+1]&0x0F;
                    }
            }
            scan_start = pos + 3 + nsc*2 + 3;
        }
        pos = seg_end;
    }

    printf("file %s: %dx%d ncomp=%d max_h=%d max_v=%d\n",
           argv[1], width, height, num_components, max_h, max_v);
    JBitReader br;
    jbr_init(&br, d + scan_start, datalen - scan_start);
    int mcu_w = 8*max_h, mcu_h = 8*max_v;
    int mcu_cx = (width+mcu_w-1)/mcu_w, mcu_cy = (height+mcu_h-1)/mcu_h;
    int prev_dc[4] = {0};

    for (int my = 0; my < mcu_cy; my++) {
        for (int mx = 0; mx < mcu_cx; mx++) {
            uint8_t comp_out[4][64*4];
            for (int c = 0; c < num_components; c++) {
                int h = comp_h[c], v = comp_v[c];
                int dc_idx = comp_dc_tbl[c];
                int ac_idx = comp_ac_tbl[c] + 2;
                for (int by = 0; by < v; by++) {
                    for (int bx = 0; bx < h; bx++) {
                        int coeffs[64] = {0};
                        int cat = jht_decode(&ht_tables[dc_idx], ht_roots[dc_idx], &br);
                        if (cat < 0 || cat > 11) return 1;
                        int diff = 0;
                        if (cat > 0) {
                            int bv = jbr_read_bits(&br, cat);
                            if (bv < 0) return 1;
                            diff = (bv & (1<<(cat-1))) ? bv : bv - ((1<<cat)-1);
                        }
                        prev_dc[c] += diff;
                        coeffs[0] = prev_dc[c];
                        int k = 1;
                        while (k < 64) {
                            int sym = jht_decode(&ht_tables[ac_idx], ht_roots[ac_idx], &br);
                            if (sym < 0) return 1;
                            if (sym == 0) break;
                            int run = (sym>>4)&0x0F, cat2 = sym&0x0F;
                            if (run==15 && cat2==0) { k+=16; continue; }
                            k += run;
                            if (k >= 64) break;
                            int bv = jbr_read_bits(&br, cat2);
                            if (bv < 0) return 1;
                            coeffs[k] = (bv & (1<<(cat2-1))) ? bv : bv - ((1<<cat2)-1);
                            k++;
                        }
                        int qid = (c==0) ? 0 : 1;
                        int dq[64];
                        for (int i=0;i<64;i++) dq[i] = coeffs[i] * (int)quant_tables[qid][i];
                        if (mx == want_mx && my == want_my) {
                            printf("MCU(%d,%d) comp %d qz=%d: ", mx, my, c, qid);
                            for (int i = 0; i < 16; i++) printf("%d ", dq[i]);
                            printf("\n");
                            uint8_t bp[64];
                            idct_2d(dq, bp);
                            printf("  idct row2: ");
                            for (int i = 16; i < 24; i++) printf("%d ", bp[i]);
                            printf("\n");
                        }
                    }
                }
            }
        }
    }
    return 0;
}
