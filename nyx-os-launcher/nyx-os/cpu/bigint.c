// cpu/bigint.c — 2048-bit unsigned big integers.
//
// Used by the TLS client for the RSA public-key operation
// (encrypting the premaster secret): c = m^e mod n with e = 65537.
// Modular exponentiation uses Montgomery multiplication (SOS form)
// with R = 2^2048.
#include <bigint.h>
#include <string.h>

// Limbs are big-endian: limb[0] holds the most significant 32 bits.
// A big-endian byte array of `len` bytes is right-aligned inside the
// 128-byte (BI_LIMBS*4) field, so byte k of the input sits at field
// position (128 - len + k).
void bi_from_bytes(uint32_t out[BI_LIMBS], const uint8_t* be, int len) {
    memset(out, 0, BI_LIMBS * 4);
    if (len > BI_LIMBS * 4) len = BI_LIMBS * 4;
    for (int k = 0; k < len; k++) {
        int pos = (BI_LIMBS * 4 - len) + k;   // field position (0 = MSB)
        int limb = pos >> 2;
        int sh = (3 - (pos & 3)) * 8;         // top byte of a limb = shift 24
        out[limb] |= (uint32_t)be[k] << sh;
    }
}

void bi_to_bytes(const uint32_t in[BI_LIMBS], uint8_t* out, int len) {
    for (int j = 0; j < len; j++) {
        int pos = (BI_LIMBS * 4 - len) + j;   // field position (0 = MSB)
        int limb = pos >> 2;
        int sh = (3 - (pos & 3)) * 8;
        out[j] = (uint8_t)(in[limb] >> sh);
    }
}

void bi_set_word(uint32_t a[BI_LIMBS], uint32_t v) {
    memset(a, 0, BI_LIMBS * 4);
    a[BI_LIMBS - 1] = v;
}

void bi_copy(uint32_t d[BI_LIMBS], const uint32_t s[BI_LIMBS]) {
    memcpy(d, s, BI_LIMBS * 4);
}

int bi_is_zero(const uint32_t a[BI_LIMBS]) {
    for (int i = 0; i < BI_LIMBS; i++)
        if (a[i]) return 0;
    return 1;
}

int bi_cmp(const uint32_t a[BI_LIMBS], const uint32_t b[BI_LIMBS]) {
    for (int i = 0; i < BI_LIMBS; i++) {
        if (a[i] != b[i]) return (a[i] > b[i]) ? 1 : -1;
    }
    return 0;
}

void bi_sub(uint32_t a[BI_LIMBS], const uint32_t b[BI_LIMBS]) {
    uint64_t borrow = 0;
    for (int i = BI_LIMBS - 1; i >= 0; i--) {
        uint64_t av = a[i];
        uint64_t diff = av - b[i] - borrow;
        a[i] = (uint32_t)diff;
        borrow = (av < b[i] + borrow) ? 1 : 0;
    }
}

// n0 such that n0 * n[31] ≡ -1 (mod 2^32) — Hensel lifting.
static uint32_t mont_n0(const uint32_t n[BI_LIMBS]) {
    uint32_t x = 1;
    uint32_t lo = n[BI_LIMBS - 1];
    for (int i = 0; i < 5; i++) x *= 2 - lo * x;   // x = n^-1 mod 2^32
    return 0u - x;                                  // -n^-1 mod 2^32
}

// Montgomery multiply: r = a*b*R^-1 mod n (a, b, n MSB-first).
// Internally works LSB-first (t[0] = least significant word), which is
// the classic SOS form: each iteration adds a[i]*b, zeroes the low word
// with m = low*n0, adds m*n, and shifts right one word.
static void bi_mont_mul(uint32_t r[BI_LIMBS], const uint32_t a_msb[BI_LIMBS],
                        const uint32_t b_msb[BI_LIMBS],
                        const uint32_t n_msb[BI_LIMBS], uint32_t n0) {
    uint32_t a[BI_LIMBS], b[BI_LIMBS], n[BI_LIMBS];
    for (int i = 0; i < BI_LIMBS; i++) {
        a[i] = a_msb[BI_LIMBS - 1 - i];
        b[i] = b_msb[BI_LIMBS - 1 - i];
        n[i] = n_msb[BI_LIMBS - 1 - i];
    }

    uint32_t t[BI_LIMBS + 2];   // LSB-first
    memset(t, 0, sizeof(t));

    for (int i = 0; i < BI_LIMBS; i++) {
        // t += a[i] * b
        uint64_t carry = 0;
        for (int j = 0; j < BI_LIMBS; j++) {
            uint64_t cur = (uint64_t)t[j] + (uint64_t)a[i] * b[j] + carry;
            t[j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        uint64_t hi = (uint64_t)t[BI_LIMBS] + carry;
        t[BI_LIMBS] = (uint32_t)hi;
        t[BI_LIMBS + 1] = (uint32_t)(hi >> 32);

        // m = low word * n0 (mod 2^32)
        uint32_t m = t[0] * n0;

        // t += m * n
        carry = 0;
        for (int j = 0; j < BI_LIMBS; j++) {
            uint64_t cur = (uint64_t)t[j] + (uint64_t)m * n[j] + carry;
            t[j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        uint64_t hi2 = (uint64_t)t[BI_LIMBS] + carry;
        t[BI_LIMBS] = (uint32_t)hi2;
        t[BI_LIMBS + 1] = (uint32_t)(hi2 >> 32);

        // t >>= 32 (exact: the low word was zeroed by the m*n step)
        for (int j = 0; j <= BI_LIMBS; j++) t[j] = t[j + 1];
        t[BI_LIMBS + 1] = 0;
    }

    // t[0..BI_LIMBS-1] is the result (< 2n); convert back to MSB-first
    // and subtract n once if needed.
    uint32_t res[BI_LIMBS];
    for (int i = 0; i < BI_LIMBS; i++) res[i] = t[BI_LIMBS - 1 - i];
    if (bi_cmp(res, n_msb) >= 0) bi_sub(res, n_msb);
    bi_copy(r, res);
}

// r = 2r mod n, for computing R^2 mod n by doubling.
// Requires r < n < 2^(BI_LIMBS*32). 2r can be up to 2^(B+1), so the
// doubling is done in a (B+1)-bit temporary; since 2r < 2n the
// reduction needs at most one subtraction of n.
static void bi_dbl_mod(uint32_t r[BI_LIMBS], const uint32_t n[BI_LIMBS]) {
    uint32_t tmp[BI_LIMBS + 1];
    uint32_t carry = 0;
    for (int i = BI_LIMBS - 1; i >= 0; i--) {
        uint64_t v = ((uint64_t)r[i] << 1) | carry;
        tmp[i + 1] = (uint32_t)v;
        carry = (uint32_t)(v >> 32);
    }
    tmp[0] = carry;                       // bit B (may be 0)
    int ge = (tmp[0] != 0) || (bi_cmp(tmp + 1, n) >= 0);
    if (ge) {                             // tmp -= n  (once suffices)
        uint64_t borrow = 0;
        for (int i = BI_LIMBS; i >= 1; i--) {
            uint64_t av = tmp[i];
            uint64_t diff = av - n[i - 1] - borrow;
            tmp[i] = (uint32_t)diff;
            borrow = (av < n[i - 1] + borrow) ? 1 : 0;
        }
        tmp[0] = 0;
    }
    for (int i = 0; i < BI_LIMBS; i++) r[i] = tmp[i + 1];
}

// R2 = R^2 mod n = 2^(2*BI_LIMBS*32) mod n.
static void bi_R2(uint32_t r2[BI_LIMBS], const uint32_t n[BI_LIMBS]) {
    bi_set_word(r2, 1);
    for (int i = 0; i < BI_LIMBS * 32; i++) bi_dbl_mod(r2, n);   // 2^B mod n
    for (int i = 0; i < BI_LIMBS * 32; i++) bi_dbl_mod(r2, n);   // ... squared
}

void bi_mod_pow(uint32_t out[BI_LIMBS], const uint32_t base[BI_LIMBS],
                const uint32_t exp[BI_LIMBS], int exp_bits,
                const uint32_t n[BI_LIMBS]) {
    if (bi_is_zero(n)) { bi_set_word(out, 0); return; }

    uint32_t n0 = mont_n0(n);
    uint32_t R2[BI_LIMBS];
    bi_R2(R2, n);

    uint32_t ba[BI_LIMBS], res[BI_LIMBS], one[BI_LIMBS];
    // ba = base * R mod n (Montgomery form of base)
    bi_mont_mul(ba, base, R2, n, n0);
    // res = 1 * R mod n = R mod n (Montgomery form of 1)
    bi_set_word(one, 1);
    bi_mont_mul(res, one, R2, n, n0);

    if (exp_bits <= 0) exp_bits = 1;
    if (exp_bits > BI_LIMBS * 32) exp_bits = BI_LIMBS * 32;

    // exp is MSB-first, so bit i (counting from the MSB) lives in
    // limb (BI_LIMBS-1 - (i>>5)) at position (i & 31).
    for (int i = exp_bits - 1; i >= 0; i--) {
        bi_mont_mul(res, res, res, n, n0);
        if ((exp[BI_LIMBS - 1 - (i >> 5)] >> (i & 31)) & 1)
            bi_mont_mul(res, res, ba, n, n0);
    }

    // out = res * R^-1 mod n = mont_mul(res, 1)
    bi_mont_mul(out, res, one, n, n0);
}
