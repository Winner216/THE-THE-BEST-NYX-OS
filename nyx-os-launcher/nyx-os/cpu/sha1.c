// cpu/sha1.c — SHA-1 (FIPS 180-1)
#include <sha1.h>
#include <string.h>

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_block(sha1_ctx* c, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = ROTL32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];

    for (int i = 0; i < 80; i++) {
        uint32_t fword, k;
        if (i < 20)      { fword = (b & cc) | ((~b) & d);   k = 0x5A827999; }
        else if (i < 40) { fword = b ^ cc ^ d;              k = 0x6ED9EBA1; }
        else if (i < 60) { fword = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { fword = b ^ cc ^ d;              k = 0xCA62C1D6; }
        uint32_t tmp = ROTL32(a, 5) + fword + e + k + w[i];
        e = d; d = cc; cc = ROTL32(b, 30); b = a; a = tmp;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sha1_init(sha1_ctx* c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->len = 0; c->buf_len = 0;
}

void sha1_update(sha1_ctx* c, const void* data, int len) {
    const uint8_t* p = (const uint8_t*)data;
    c->len += len;
    if (c->buf_len) {
        int take = 64 - c->buf_len;
        if (take > len) take = len;
        memcpy(c->buf + c->buf_len, p, take);
        c->buf_len += take;
        p += take; len -= take;
        if (c->buf_len == 64) { sha1_block(c, c->buf); c->buf_len = 0; }
    }
    while (len >= 64) {
        sha1_block(c, p);
        p += 64; len -= 64;
    }
    if (len) {
        memcpy(c->buf, p, len);
        c->buf_len = len;
    }
}

void sha1_final(sha1_ctx* c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buf_len != 56) sha1_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

void sha1_hash(const void* data, int len, uint8_t out[20]) {
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}
