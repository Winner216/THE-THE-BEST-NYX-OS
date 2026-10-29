// cpu/aes.c — AES-128 (FIPS-197) with CBC mode.
// Compact implementation with a fixed S-box; used by the TLS client.
#include <aes.h>
#include <string.h>

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

// GF(2^8) multiply (used in inverse MixColumns)
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

void aes128_setkey(aes128_ctx* c, const uint8_t key[16]) {
    uint32_t* w = c->rk;
    for (int i = 0; i < 4; i++)
        w[i] = ((uint32_t)key[i*4] << 24) | ((uint32_t)key[i*4+1] << 16) |
               ((uint32_t)key[i*4+2] << 8) | key[i*4+3];

    static const uint8_t rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

    for (int i = 4; i < 44; i++) {
        uint32_t t = w[i-1];
        if ((i & 3) == 0) {
            // RotWord (rotate left one byte) + SubWord + Rcon.
            // t = B0 B1 B2 B3 (MSB..LSB); rotated = B1 B2 B3 B0.
            uint8_t b0 = sbox[(t >> 16) & 0xff],  // old B1 -> new MSB
                    b1 = sbox[(t >> 8) & 0xff],   // old B2
                    b2 = sbox[t & 0xff],          // old B3 (LSB)
                    b3 = sbox[(t >> 24) & 0xff];  // old B0 -> new LSB
            t = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
                ((uint32_t)b2 << 8) | b3;
            t ^= ((uint32_t)rcon[i/4 - 1]) << 24;
        }
        w[i] = w[i-4] ^ t;
    }
}

static void add_round_key(uint8_t s[16], const uint32_t* rk) {
    for (int i = 0; i < 4; i++) {
        uint32_t k = rk[i];
        s[i*4]   ^= (uint8_t)(k >> 24);
        s[i*4+1] ^= (uint8_t)(k >> 16);
        s[i*4+2] ^= (uint8_t)(k >> 8);
        s[i*4+3] ^= (uint8_t)k;
    }
}

void aes128_encrypt(const aes128_ctx* c, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, c->rk);

    for (int r = 1; r < 10; r++) {
        // SubBytes
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        // ShiftRows
        uint8_t t[16];
        t[0]=s[0]; t[1]=s[5]; t[2]=s[10]; t[3]=s[15];
        t[4]=s[4]; t[5]=s[9]; t[6]=s[14]; t[7]=s[3];
        t[8]=s[8]; t[9]=s[13]; t[10]=s[2]; t[11]=s[7];
        t[12]=s[12]; t[13]=s[1]; t[14]=s[6]; t[15]=s[11];
        // MixColumns
        for (int col = 0; col < 4; col++) {
            uint8_t a0 = t[col*4], a1 = t[col*4+1], a2 = t[col*4+2], a3 = t[col*4+3];
            s[col*4]   = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
            s[col*4+1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
            s[col*4+2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
            s[col*4+3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
        }
        add_round_key(s, c->rk + r*4);
    }

    // Final round (no MixColumns)
    for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
    uint8_t t[16];
    t[0]=s[0]; t[1]=s[5]; t[2]=s[10]; t[3]=s[15];
    t[4]=s[4]; t[5]=s[9]; t[6]=s[14]; t[7]=s[3];
    t[8]=s[8]; t[9]=s[13]; t[10]=s[2]; t[11]=s[7];
    t[12]=s[12]; t[13]=s[1]; t[14]=s[6]; t[15]=s[11];
    memcpy(s, t, 16);
    add_round_key(s, c->rk + 40);
    memcpy(out, s, 16);
}

void aes128_decrypt(const aes128_ctx* c, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, c->rk + 40);

    for (int r = 9; r >= 1; r--) {
        // InvShiftRows: row i shifts right by i, i.e. out[c] = in[(c+4-i)%4]
        uint8_t t[16];
        t[0]=s[0]; t[4]=s[4]; t[8]=s[8]; t[12]=s[12];                 // row 0
        t[1]=s[13]; t[5]=s[1]; t[9]=s[5]; t[13]=s[9];                 // row 1
        t[2]=s[10]; t[6]=s[14]; t[10]=s[2]; t[14]=s[6];               // row 2
        t[3]=s[7]; t[7]=s[11]; t[11]=s[15]; t[15]=s[3];               // row 3
        // InvSubBytes
        for (int i = 0; i < 16; i++) t[i] = rsbox[t[i]];
        // AddRoundKey BEFORE InvMixColumns (inverse-cipher order)
        add_round_key(t, c->rk + r*4);
        // InvMixColumns
        for (int col = 0; col < 4; col++) {
            uint8_t a0 = t[col*4], a1 = t[col*4+1], a2 = t[col*4+2], a3 = t[col*4+3];
            s[col*4]   = (uint8_t)(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3,9));
            s[col*4+1] = (uint8_t)(gmul(a0,9)  ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
            s[col*4+2] = (uint8_t)(gmul(a0,13) ^ gmul(a1,9)  ^ gmul(a2,14) ^ gmul(a3,11));
            s[col*4+3] = (uint8_t)(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2,9)  ^ gmul(a3,14));
        }
    }

    // Final: InvShiftRows + InvSubBytes + AddRoundKey(0)
    uint8_t t[16];
    t[0]=s[0]; t[4]=s[4]; t[8]=s[8]; t[12]=s[12];
    t[1]=s[13]; t[5]=s[1]; t[9]=s[5]; t[13]=s[9];
    t[2]=s[10]; t[6]=s[14]; t[10]=s[2]; t[14]=s[6];
    t[3]=s[7]; t[7]=s[11]; t[11]=s[15]; t[15]=s[3];
    for (int i = 0; i < 16; i++) t[i] = rsbox[t[i]];
    memcpy(s, t, 16);
    add_round_key(s, c->rk);
    memcpy(out, s, 16);
}

void aes128_cbc_encrypt(const aes128_ctx* c, const uint8_t* in, uint8_t* out,
                        int len, uint8_t iv[16]) {
    uint8_t prev[16], blk[16];
    memcpy(prev, iv, 16);
    for (int off = 0; off < len; off += 16) {
        for (int i = 0; i < 16; i++) blk[i] = in[off+i] ^ prev[i];
        aes128_encrypt(c, blk, out + off);
        memcpy(prev, out + off, 16);
    }
    memcpy(iv, prev, 16);   // last ciphertext block becomes next IV
}

void aes128_cbc_decrypt(const aes128_ctx* c, const uint8_t* in, uint8_t* out,
                        int len, const uint8_t iv[16]) {
    uint8_t prev[16], cur[16];
    memcpy(prev, iv, 16);
    for (int off = 0; off < len; off += 16) {
        aes128_decrypt(c, in + off, out + off);
        for (int i = 0; i < 16; i++) out[off+i] ^= prev[i];
        memcpy(prev, in + off, 16);
    }
}

// ─── AES-128-GCM (NIST SP 800-38D) ──────────────────────────────
// GF(2^128) multiply (bit-serial) with the GCM reduction polynomial
// x^128 + x^7 + x^2 + x + 1.  Big-endian 16-byte operands.
static void gf128_mul(const uint8_t x[16], const uint8_t h[16], uint8_t out[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    memcpy(v, h, 16);
    for (int i = 127; i >= 0; i--) {
        int xbit = (x[15 - (i >> 3)] >> (i & 7)) & 1;
        if (xbit) {
            for (int k = 0; k < 16; k++) z[k] ^= v[k];
        }
        // v >>= 1 with polynomial feedback
        int lsb = v[15] & 1;
        for (int k = 15; k > 0; k--) v[k] = (uint8_t)((v[k] >> 1) | (v[k - 1] << 7));
        v[0] >>= 1;
        if (lsb) {
            v[0] ^= 0xE1;   // x^7 + x^2 + x + 1 shifted to the top byte
        }
    }
    memcpy(out, z, 16);
}

// GHASH over AAD then ciphertext (each padded to 16) + length block.
static void ghash(const uint8_t h[16], const uint8_t* aad, int aad_len,
                  const uint8_t* ct, int ct_len, uint8_t out[16]) {
    uint8_t y[16] = {0};
    uint8_t blk[16];
    int off = 0;
    while (off < aad_len) {
        int n = aad_len - off;
        if (n > 16) n = 16;
        memset(blk, 0, 16);
        memcpy(blk, aad + off, n);
        for (int i = 0; i < 16; i++) y[i] ^= blk[i];
        gf128_mul(y, h, y);
        off += n;
    }
    off = 0;
    while (off < ct_len) {
        int n = ct_len - off;
        if (n > 16) n = 16;
        memset(blk, 0, 16);
        memcpy(blk, ct + off, n);
        for (int i = 0; i < 16; i++) y[i] ^= blk[i];
        gf128_mul(y, h, y);
        off += n;
    }
    // length block: 8-byte BE bit-lengths
    uint64_t abits = (uint64_t)aad_len * 8;
    uint64_t cbits = (uint64_t)ct_len * 8;
    memset(blk, 0, 16);
    for (int i = 0; i < 8; i++) {
        blk[i] = (uint8_t)(abits >> (56 - 8 * i));
        blk[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    }
    for (int i = 0; i < 16; i++) y[i] ^= blk[i];
    gf128_mul(y, h, y);
    memcpy(out, y, 16);
}

void aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t* aad, int aad_len,
                     const uint8_t* in, int in_len,
                     uint8_t* out, uint8_t tag[16]) {
    aes128_ctx c;
    aes128_setkey(&c, key);

    // H = E_K(0^128)
    uint8_t h[16] = {0};
    aes128_encrypt(&c, h, h);

    // J0 = IV || 0^31 || 1
    uint8_t j0[16];
    memcpy(j0, iv, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    // CTR blocks: C_i = P_i xor E_K(J0 + i)
    uint8_t ctr[16];
    memcpy(ctr, j0, 16);
    int off = 0;
    while (off < in_len) {
        // increment the counter part (last 4 bytes, big-endian)
        for (int i = 15; i >= 12; i--) {
            if (++ctr[i] != 0) break;
        }
        uint8_t keystream[16];
        aes128_encrypt(&c, ctr, keystream);
        int n = in_len - off;
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) out[off + i] = in[off + i] ^ keystream[i];
        off += n;
    }

    // tag = GHASH(AAD, C) xor E_K(J0)
    uint8_t s[16];
    ghash(h, aad, aad_len, out, in_len, s);
    uint8_t e0[16];
    aes128_encrypt(&c, j0, e0);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ e0[i];
}

int aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t* aad, int aad_len,
                    const uint8_t* in, int in_len,
                    uint8_t* out, const uint8_t tag[16]) {
    aes128_ctx c;
    aes128_setkey(&c, key);
    uint8_t h[16] = {0};
    aes128_encrypt(&c, h, h);

    // Expected tag = GHASH(AAD, received ciphertext) xor E_K(J0)
    uint8_t s[16];
    ghash(h, aad, aad_len, in, in_len, s);
    uint8_t j0[16];
    memcpy(j0, iv, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    uint8_t e0[16];
    aes128_encrypt(&c, j0, e0);
    int bad = 0;
    for (int i = 0; i < 16; i++)
        if ((s[i] ^ e0[i]) != tag[i]) bad = 1;
    if (bad) return 0;

    // decrypt (CTR)
    uint8_t ctr[16];
    memcpy(ctr, j0, 16);
    int off = 0;
    while (off < in_len) {
        for (int i = 15; i >= 12; i--) {
            if (++ctr[i] != 0) break;
        }
        uint8_t ks[16];
        aes128_encrypt(&c, ctr, ks);
        int n = in_len - off;
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) out[off + i] = in[off + i] ^ ks[i];
        off += n;
    }
    return 1;
}
