// tools/_jpeg_inc_test.c — includes the REAL cpu/jpegdec.c so the
// actual static idct_2d runs, then feeds it the dequantized
// coefficients of a target MCU and prints the output block.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../cpu/jpegdec.c"

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc(sz); if (fread(d, 1, sz, f) != (size_t)sz) return 2;
    fclose(f);
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
                            uint8_t bp[64];
                            idct_2d(dq, bp);   // the REAL idct from jpegdec.c
                            printf("MCU(%d,%d) comp %d: dq: ", mx, my, c);
                            for (int i = 0; i < 64; i++) printf("%d ", dq[i]);
                            printf("\nMCU(%d,%d) comp %d: idct: ", mx, my, c);
                            for (int i = 0; i < 64; i++) printf("%d ", bp[i]);
                            printf("\n");
                        }
                    }
                }
            }
        }
    }
    return 0;
}
