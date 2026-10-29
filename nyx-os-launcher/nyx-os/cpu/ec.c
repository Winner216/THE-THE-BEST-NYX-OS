// cpu/ec.c — NIST P-256 elliptic-curve math for TLS ECDHE/ECDSA.
//
// Representation: 8 x 32-bit limbs, little-endian word order (limb[0]
// is the least significant).  All byte-level inputs/outputs (scalars,
// points, digests) are big-endian to match the TLS wire format.
//
// Modular multiplication uses Barrett reduction with mu computed once
// per modulus (P-256 prime p and group order n).  Point arithmetic is
// in Jacobian coordinates (x = X/Z^2, y = Y/Z^3) with a single field
// inversion at the end, using the standard formulas:
//   dbl-2009-l  (a = -3) and add-2007-bl.
// ECDSA verification: u1 = e*w, u2 = r*w, R = u1*G + u2*Q,
// accept if R.x mod n == r (ANSI X9.62).

#include <ec.h>
#include <string.h>

// ─── Constants (big-endian) ───────────────────────────────────────
static const uint8_t P_BE[32] = {
    0xFF,0xFF,0xFF,0xFF, 0x00,0x00,0x00,0x01, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF
};
static const uint8_t N_BE[32] = {
    0xFF,0xFF,0xFF,0xFF, 0x00,0x00,0x00,0x00, 0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF, 0xBC,0xE6,0xFA,0xAD, 0xA7,0x17,0x9E,0x84,
    0xF3,0xB9,0xCA,0xC2, 0xFC,0x63,0x25,0x51
};
static const uint8_t GX_BE[32] = {
    0x6B,0x17,0xD1,0xF2, 0xE1,0x2C,0x42,0x47, 0xF8,0xBC,0xE6,0xE5,
    0x63,0xA4,0x40,0xF2, 0x77,0x03,0x7D,0x81, 0x2D,0xEB,0x33,0xA0,
    0xF4,0xA1,0x39,0x45, 0xD8,0x98,0xC2,0x96
};
static const uint8_t GY_BE[32] = {
    0x4F,0xE3,0x42,0xE2, 0xFE,0x1A,0x7F,0x9B, 0x8E,0xE7,0xEB,0x4A,
    0x7C,0x0F,0x9E,0x16, 0x2B,0xCE,0x33,0x57, 0x6B,0x31,0x5E,0xCE,
    0xCB,0xB6,0x40,0x68, 0x37,0xBF,0x51,0xF5
};
static const uint8_t B_BE[32] = {
    0x5A,0xC6,0x35,0xD8, 0xAA,0x3A,0x93,0xE7, 0xB3,0xEB,0xBD,0x55,
    0x76,0x98,0x86,0xBC, 0x65,0x1D,0x06,0xB0, 0xCC,0x53,0xB0,0xF6,
    0x3B,0xCE,0x3C,0x3E, 0x27,0xD2,0x60,0x4B
};
static const uint8_t A_BE[32] = {   // p - 3
    0xFF,0xFF,0xFF,0xFF, 0x00,0x00,0x00,0x01, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFC
};

#define LIMBS 8

static uint32_t P[LIMBS], N[LIMBS], A[LIMBS], BC[LIMBS];
static uint32_t GX[LIMBS], GY[LIMBS];
static uint32_t MU_P[LIMBS + 1], MU_N[LIMBS + 1];
static int      ec_inited = 0;

// ─── Limb helpers ─────────────────────────────────────────────────
static void bytes_to_limbs(uint32_t out[LIMBS], const uint8_t* be) {
    for (int i = 0; i < LIMBS; i++) {
        int o = 28 - i * 4;          // MSB byte index of limb i
        out[i] = ((uint32_t)be[o] << 24) | ((uint32_t)be[o + 1] << 16) |
                 ((uint32_t)be[o + 2] << 8) | be[o + 3];
    }
}
static void limbs_to_bytes(const uint32_t in[LIMBS], uint8_t* be) {
    for (int i = 0; i < LIMBS; i++) {
        int o = 28 - i * 4;          // MSB byte index of limb i
        be[o]     = (uint8_t)(in[i] >> 24);
        be[o + 1] = (uint8_t)(in[i] >> 16);
        be[o + 2] = (uint8_t)(in[i] >> 8);
        be[o + 3] = (uint8_t)in[i];
    }
}

static int cmp_n(const uint32_t* a, const uint32_t* b, int n) {
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static int is_zero_n(const uint32_t* a, int n) {
    for (int i = 0; i < n; i++) if (a[i]) return 0;
    return 1;
}
// a -= b over n limbs; returns 1 if a went negative (borrow out).
static int sub_n(uint32_t* a, const uint32_t* b, int n) {
    uint32_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (uint32_t)((uint64_t)d >> 32) & 1;
    }
    return borrow;
}
// a += b over n limbs; returns 1 on carry out.
static int add_n(uint32_t* a, const uint32_t* b, int n) {
    uint32_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        a[i] = (uint32_t)s;
        carry = (uint32_t)(s >> 32);
    }
    return carry;
}

// ─── Schoolbook multiply (16 limbs) ──────────────────────────────
static void mul_8x8(const uint32_t* a, const uint32_t* b, uint32_t out[16]) {
    memset(out, 0, 16 * sizeof(uint32_t));
    for (int i = 0; i < LIMBS; i++) {
        uint32_t carry = 0;
        for (int j = 0; j < LIMBS; j++) {
            uint64_t t = (uint64_t)a[i] * b[j] + out[i + j] + carry;
            out[i + j] = (uint32_t)t;
            carry = (uint32_t)(t >> 32);
        }
        out[i + LIMBS] = carry;
    }
}

// ─── Barrett constants ────────────────────────────────────────────
// mu = floor(2^512 / m), computed by shift-subtract long division.
// Result is LIMBS+1 limbs (fits in 9 since m >= 2^255).
static void barrett_mu(const uint32_t m[LIMBS], uint32_t mu[LIMBS + 1]) {
    uint32_t r[LIMBS + 1];          // remainder
    memset(r, 0, sizeof(r));
    memset(mu, 0, (LIMBS + 1) * sizeof(uint32_t));
    // dividend = 2^512 : bit 512 set.  Loop i = 512..0.
    for (int i = 512; i >= 0; i--) {
        // r = r << 1
        uint32_t carry = 0;
        for (int k = 0; k <= LIMBS; k++) {
            uint32_t nr = (r[k] << 1) | carry;
            carry = r[k] >> 31;
            r[k] = nr;
        }
        if (i == 512) r[0] |= 1;     // dividend bit 0 lands in r[0]
        // if r >= m, subtract m and set quotient bit i.  r can hold a
        // bit in limb LIMBS (r < 2m < 2^257), so the compare must
        // include it and the subtract must span LIMBS+1 limbs.
        int ge = (r[LIMBS] != 0) || (cmp_n(r, m, LIMBS) >= 0);
        if (ge) {
            uint32_t me[LIMBS + 1];
            memcpy(me, m, LIMBS * sizeof(uint32_t));
            me[LIMBS] = 0;
            sub_n(r, me, LIMBS + 1);
            int limb = i / 32, bit = i % 32;
            if (limb <= LIMBS) mu[limb] |= (1u << bit);
        }
    }
}

// Barrett reduction of a 16-limb value into 8 limbs (mod m).
static void barrett_reduce(uint32_t* t, const uint32_t m[LIMBS],
                           const uint32_t mu[LIMBS + 1], uint32_t out[LIMBS]) {
    // q1 = t[8..15]
    // q2 = q1 * mu  (17 limbs)
    uint32_t q2[17];
    memset(q2, 0, sizeof(q2));
    for (int i = 0; i < LIMBS; i++) {          // q1[i] = t[8+i]
        uint32_t carry = 0;
        for (int j = 0; j <= LIMBS; j++) {     // mu[j]
            uint64_t s = (uint64_t)t[8 + i] * mu[j] + q2[i + j] + carry;
            q2[i + j] = (uint32_t)s;
            carry = (uint32_t)(s >> 32);
        }
        q2[i + LIMBS + 1] = carry;
    }
    // q3 = q2 >> 256 = limbs 8..16 (9 limbs; limb 8 can be nonzero).
    // r = t - q3*m  (17 limbs; cannot underflow by the Barrett bound
    // q3*m <= q1*2^256 <= t).
    uint32_t r[17], q3m[17];
    memset(r, 0, sizeof(r));
    memcpy(r, t, 16 * sizeof(uint32_t));
    memset(q3m, 0, sizeof(q3m));
    for (int i = 0; i <= LIMBS; i++) {         // q3[i] = q2[8+i]
        uint32_t carry = 0;
        for (int j = 0; j < LIMBS; j++) {
            uint64_t s = (uint64_t)q2[8 + i] * m[j] + q3m[i + j] + carry;
            q3m[i + j] = (uint32_t)s;
            carry = (uint32_t)(s >> 32);
        }
        q3m[i + LIMBS] = carry;
    }
    sub_n(r, q3m, 17);
    // reduce: r < 4m, so only limbs 0..8 can be nonzero.  Subtract m
    // over the full width so a borrow out of limb 8 propagates.
    uint32_t me17[17];
    memset(me17, 0, sizeof(me17));
    memcpy(me17, m, LIMBS * sizeof(uint32_t));
    for (;;) {
        int ge = 0;
        for (int i = 16; i >= LIMBS; i--) if (r[i]) { ge = 1; break; }
        if (!ge && cmp_n(r, m, LIMBS) < 0) break;
        sub_n(r, me17, 17);
    }
    memcpy(out, r, LIMBS * sizeof(uint32_t));
}

// out = a*b mod m  (Barrett)
static void mulmod(const uint32_t a[LIMBS], const uint32_t b[LIMBS],
                   const uint32_t m[LIMBS], const uint32_t mu[LIMBS + 1],
                   uint32_t out[LIMBS]) {
    uint32_t t[16];
    mul_8x8(a, b, t);
    barrett_reduce(t, m, mu, out);
}

// ─── Field ops mod p ──────────────────────────────────────────────
static void f_add(const uint32_t a[LIMBS], const uint32_t b[LIMBS],
                  uint32_t out[LIMBS]) {
    memcpy(out, a, LIMBS * sizeof(uint32_t));
    if (add_n(out, b, LIMBS)) sub_n(out, P, LIMBS);
    if (cmp_n(out, P, LIMBS) >= 0) sub_n(out, P, LIMBS);
}
static void f_sub(const uint32_t a[LIMBS], const uint32_t b[LIMBS],
                  uint32_t out[LIMBS]) {
    memcpy(out, a, LIMBS * sizeof(uint32_t));
    if (sub_n(out, b, LIMBS)) add_n(out, P, LIMBS);
}
static void f_mul(const uint32_t a[LIMBS], const uint32_t b[LIMBS],
                  uint32_t out[LIMBS]) {
    mulmod(a, b, P, MU_P, out);
}
static void f_sqr(const uint32_t a[LIMBS], uint32_t out[LIMBS]) {
    mulmod(a, a, P, MU_P, out);
}
// out = 2*x mod p
static void f_dbl(const uint32_t x[LIMBS], uint32_t out[LIMBS]) {
    uint32_t t[LIMBS];
    memcpy(t, x, sizeof(t));
    int carry = 0;
    for (int i = 0; i < LIMBS; i++) {
        uint64_t s = (uint64_t)t[i] * 2 + carry;
        t[i] = (uint32_t)s;
        carry = (uint32_t)(s >> 32);
    }
    if (carry) sub_n(t, P, LIMBS);
    if (cmp_n(t, P, LIMBS) >= 0) sub_n(t, P, LIMBS);
    memcpy(out, t, sizeof(t));
}
// out = 3*x mod p
static void f_tri(const uint32_t x[LIMBS], uint32_t out[LIMBS]) {
    uint32_t t[LIMBS];
    f_dbl(x, t);
    f_add(t, x, out);
}

// a^e mod m (square-and-multiply; e is 256-bit big-endian bytes)
static void mod_pow_bytes(const uint32_t* base, const uint8_t* e_be,
                          const uint32_t m[LIMBS], const uint32_t mu[LIMBS + 1],
                          uint32_t out[LIMBS]) {
    uint32_t res[LIMBS], b[LIMBS];
    memcpy(b, base, sizeof(b));
    memset(res, 0, sizeof(res));
    res[0] = 1;
    for (int i = 0; i < 32; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            mulmod(res, res, m, mu, res);
            if ((e_be[i] >> bit) & 1) mulmod(res, b, m, mu, res);
        }
    }
    memcpy(out, res, sizeof(res));
}

// Modular inverse via Fermat: a^(m-2) mod m (m prime).
static void f_inv(const uint32_t a[LIMBS], const uint32_t m[LIMBS],
                  const uint32_t mu[LIMBS + 1], uint32_t out[LIMBS]) {
    uint8_t e[32];
    for (int i = 0; i < 32; i++) {
        e[i] = (uint8_t)(m[7 - (i / 4)] >> ((3 - (i % 4)) * 8));
    }
    // e = m - 2  (big-endian bytes: subtract from the LSB end)
    uint32_t c = 2;
    for (int i = 31; i >= 0 && c; i--) {
        uint8_t old = e[i];
        e[i] = (uint8_t)(e[i] - c);
        c = (c > old) ? 1 : 0;
    }
    mod_pow_bytes(a, e, m, mu, out);
}

void ec_p256_init(void) {
    if (ec_inited) return;
    bytes_to_limbs(P, P_BE);
    bytes_to_limbs(N, N_BE);
    bytes_to_limbs(A, A_BE);
    bytes_to_limbs(BC, B_BE);
    bytes_to_limbs(GX, GX_BE);
    bytes_to_limbs(GY, GY_BE);
    barrett_mu(P, MU_P);
    barrett_mu(N, MU_N);
    ec_inited = 1;
}

// ─── Point arithmetic (Jacobian) ──────────────────────────────────
typedef struct { uint32_t X[LIMBS], Y[LIMBS], Z[LIMBS]; } jac;

static void jac_dbl(const jac* a, jac* o) {
    uint32_t A[LIMBS], B[LIMBS], C[LIMBS], D[LIMBS], E[LIMBS];
    uint32_t F[LIMBS], t1[LIMBS], t2[LIMBS], t3[LIMBS];
    f_sqr(a->X, A);
    f_sqr(a->Y, B);
    f_sqr(B, C);
    f_add(a->X, B, t1);
    f_sqr(t1, t1);
    f_add(A, C, t2);
    f_sub(t1, t2, t1);          // (X+B)^2 - A - C
    f_dbl(t1, D);               // D = 2*...
    f_sqr(a->Z, t1);
    f_sub(a->X, t1, t2);        // X - Z^2
    f_add(a->X, t1, t3);        // X + Z^2
    f_mul(t2, t3, t2);
    f_tri(t2, E);               // E = 3*(X-Z^2)(X+Z^2)
    f_sqr(E, F);
    f_dbl(D, t1);
    f_sub(F, t1, o->X);         // X3 = F - 2D
    f_sub(D, o->X, t1);
    f_mul(E, t1, t1);
    f_dbl(C, t2);
    f_dbl(t2, t2);
    f_dbl(t2, t2);              // 8C
    f_sub(t1, t2, o->Y);        // Y3 = E*(D-X3) - 8C
    f_mul(a->Y, a->Z, o->Z);
    f_dbl(o->Z, o->Z);          // Z3 = 2*Y1*Z1
}

// General Jacobian addition (handles P == Q via doubling, P == -Q via
// infinity marker in Z = 0).
static void jac_add(const jac* a, const jac* b, jac* o) {
    uint32_t z1z1[LIMBS], z2z2[LIMBS], u1[LIMBS], u2[LIMBS];
    uint32_t s1[LIMBS], s2[LIMBS], h[LIMBS], i_[LIMBS], j_[LIMBS];
    uint32_t r[LIMBS], v[LIMBS], t[LIMBS], t2[LIMBS];
    int a_inf = is_zero_n(a->Z, LIMBS);
    int b_inf = is_zero_n(b->Z, LIMBS);
    if (a_inf) { *o = *b; return; }
    if (b_inf) { *o = *a; return; }

    f_sqr(a->Z, z1z1);
    f_sqr(b->Z, z2z2);
    f_mul(a->X, z2z2, u1);
    f_mul(b->X, z1z1, u2);
    f_mul(a->Y, b->Z, t);
    f_mul(t, z2z2, s1);          // S1 = Y1*Z2*Z2^2
    f_mul(b->Y, a->Z, t);
    f_mul(t, z1z1, s2);          // S2 = Y2*Z1*Z1^2
    f_sub(u2, u1, h);
    int h_zero = is_zero_n(h, LIMBS);
    if (h_zero) {
        int s_eq = cmp_n(s1, s2, LIMBS) == 0;
        if (s_eq) { jac_dbl(a, o); return; }
        memset(o->Z, 0, sizeof(o->Z));   // infinity
        return;
    }
    f_dbl(h, t);
    f_sqr(t, i_);                // I = (2H)^2
    f_mul(h, i_, j_);            // J = H*I
    f_sub(s2, s1, r);
    f_dbl(r, r);                 // r = 2*(S2-S1)
    f_mul(u1, i_, v);            // V = U1*I
    f_sqr(r, t);
    f_dbl(v, t2);
    f_sub(t, j_, t);
    f_sub(t, t2, o->X);          // X3 = r^2 - J - 2V
    f_sub(v, o->X, t);
    f_mul(r, t, t);
    f_mul(s1, j_, t2);
    f_dbl(t2, t2);
    f_sub(t, t2, o->Y);          // Y3 = r*(V-X3) - 2*S1*J
    f_add(a->Z, b->Z, t);
    f_sqr(t, t);
    f_sub(t, z1z1, t);
    f_sub(t, z2z2, t);
    f_mul(t, h, o->Z);           // Z3 = ((Z1+Z2)^2 - Z1Z1 - Z2Z2)*H
}

// Jacobian -> affine (inverts Z).  out[0..7]=x, out[8..15]=y limbs.
static void jac_to_affine(const jac* a, uint32_t x[LIMBS], uint32_t y[LIMBS]) {
    if (is_zero_n(a->Z, LIMBS)) {
        memset(x, 0, LIMBS * sizeof(uint32_t));
        memset(y, 0, LIMBS * sizeof(uint32_t));
        return;
    }
    uint32_t zi[LIMBS], z2[LIMBS], z3[LIMBS];
    f_inv(a->Z, P, MU_P, zi);
    f_sqr(zi, z2);
    f_mul(z2, zi, z3);
    f_mul(a->X, z2, x);
    f_mul(a->Y, z3, y);
}

// k*P: double-and-add, MSB first.  P is affine (limbs).
static void scalar_mult(const uint32_t k[LIMBS], const uint32_t px[LIMBS],
                        const uint32_t py[LIMBS], uint32_t out_x[LIMBS],
                        uint32_t out_y[LIMBS]) {
    jac base, acc;
    memcpy(base.X, px, sizeof(base.X));
    memcpy(base.Y, py, sizeof(base.Y));
    memset(base.Z, 0, sizeof(base.Z));
    base.Z[0] = 1;
    memset(&acc, 0, sizeof(acc));      // acc = infinity (Z = 0)
    int started = 0;
    jac tmp;
    for (int i = 255; i >= 0; i--) {
        if (started) { jac_dbl(&acc, &tmp); acc = tmp; }
        int bit = (k[i / 32] >> (i % 32)) & 1;
        if (bit) {
            if (started) { jac_add(&acc, &base, &tmp); acc = tmp; }
            else { acc = base; started = 1; }
        }
    }
    if (!started) {
        memset(out_x, 0, LIMBS * sizeof(uint32_t));
        memset(out_y, 0, LIMBS * sizeof(uint32_t));
        return;
    }
    jac_to_affine(&acc, out_x, out_y);
}

// ─── Public API ───────────────────────────────────────────────────
void ec_p256_scalar_mult_g(const uint8_t k[32], uint8_t out[64]) {
    ec_p256_init();
    uint32_t kl[LIMBS];
    bytes_to_limbs(kl, k);
    uint32_t x[LIMBS], y[LIMBS];
    scalar_mult(kl, GX, GY, x, y);
    limbs_to_bytes(x, out);
    limbs_to_bytes(y, out + 32);
}

int ec_p256_on_curve(const uint8_t P_[64]) {
    ec_p256_init();
    uint32_t x[LIMBS], y[LIMBS];
    bytes_to_limbs(x, P_);
    bytes_to_limbs(y, P_ + 32);
    uint32_t t1[LIMBS], t2[LIMBS];
    f_sqr(x, t1);                  // x^2
    f_mul(t1, x, t1);              // x^3
    f_mul(A, x, t2);
    f_add(t1, t2, t1);
    f_add(t1, BC, t1);             // x^3 + ax + b
    f_sqr(y, t2);
    return cmp_n(t1, t2, LIMBS) == 0;
}

int ec_p256_scalar_mult(const uint8_t k[32], const uint8_t P_[64],
                        uint8_t out[64]) {
    ec_p256_init();
    if (!ec_p256_on_curve(P_)) return -1;
    uint32_t kl[LIMBS], px[LIMBS], py[LIMBS];
    bytes_to_limbs(kl, k);
    bytes_to_limbs(px, P_);
    bytes_to_limbs(py, P_ + 32);
    uint32_t x[LIMBS], y[LIMBS];
    scalar_mult(kl, px, py, x, y);
    limbs_to_bytes(x, out);
    limbs_to_bytes(y, out + 32);
    return 0;
}

// ─── ECDSA ────────────────────────────────────────────────────────
// Parse DER SEQUENCE { INTEGER r, INTEGER s }.  Returns 1 on success.
static int der_parse_ecdsa(const uint8_t* d, int len,
                           uint8_t r[32], uint8_t s[32]) {
    if (len < 8 || d[0] != 0x30) return 0;
    int p = 2;                       // skip SEQUENCE tag + length byte
    if (d[1] & 0x80) {               // long-form length
        int nl = d[1] & 0x7F;
        if (nl > 2 || 2 + nl > len) return 0;
        p = 2 + nl;
    }
    if (p + 1 >= len || d[p] != 0x02) return 0;
    int rl = d[p + 1];
    if (rl < 1 || rl > 33 || p + 2 + rl > len) return 0;
    const uint8_t* rv = d + p + 2;
    int rs = rl;
    if (rv[0] == 0x00) { rv++; rs--; }
    if (rs < 1 || rs > 32) return 0;
    memset(r, 0, 32);
    memcpy(r + 32 - rs, rv, rs);
    p += 2 + rl;
    if (p + 1 >= len || d[p] != 0x02) return 0;
    int sl = d[p + 1];
    if (sl < 1 || sl > 33 || p + 2 + sl > len) return 0;
    const uint8_t* sv = d + p + 2;
    int ss = sl;
    if (sv[0] == 0x00) { sv++; ss--; }
    if (ss < 1 || ss > 32) return 0;
    memset(s, 0, 32);
    memcpy(s + 32 - ss, sv, ss);
    return 1;
}

int ec_p256_verify(const uint8_t Q[64], const uint8_t digest[32],
                   const uint8_t* der_sig, int sig_len) {
    ec_p256_init();
    uint8_t r[32], s[32];
    if (!der_parse_ecdsa(der_sig, sig_len, r, s)) return 0;

    // r, s in (0, n)
    uint32_t rl[LIMBS], sl[LIMBS], el[LIMBS];
    uint32_t one[LIMBS] = {1}, zero[LIMBS] = {0};
    bytes_to_limbs(rl, r);
    bytes_to_limbs(sl, s);
    if (cmp_n(rl, zero, LIMBS) <= 0 || cmp_n(rl, N, LIMBS) >= 0) return 0;
    if (cmp_n(sl, zero, LIMBS) <= 0 || cmp_n(sl, N, LIMBS) >= 0) return 0;

    // e = digest mod n
    bytes_to_limbs(el, digest);
    if (cmp_n(el, N, LIMBS) >= 0) {
        mulmod(el, one, N, MU_N, el);  // reduce mod n
    }

    // w = s^-1 mod n
    uint32_t w[LIMBS];
    f_inv(sl, N, MU_N, w);
    // u1 = e*w mod n, u2 = r*w mod n
    uint32_t u1[LIMBS], u2[LIMBS];
    mulmod(el, w, N, MU_N, u1);
    mulmod(rl, w, N, MU_N, u2);

    // R = u1*G + u2*Q
    uint32_t r1x[LIMBS], r1y[LIMBS], r2x[LIMBS], r2y[LIMBS];
    scalar_mult(u1, GX, GY, r1x, r1y);
    uint32_t qx[LIMBS], qy[LIMBS];
    bytes_to_limbs(qx, Q);
    bytes_to_limbs(qy, Q + 32);
    scalar_mult(u2, qx, qy, r2x, r2y);

    jac j1, j2;
    memcpy(j1.X, r1x, sizeof(j1.X));
    memcpy(j1.Y, r1y, sizeof(j1.Y));
    memset(j1.Z, 0, sizeof(j1.Z));
    j1.Z[0] = 1;
    memcpy(j2.X, r2x, sizeof(j2.X));
    memcpy(j2.Y, r2y, sizeof(j2.Y));
    memset(j2.Z, 0, sizeof(j2.Z));
    j2.Z[0] = 1;
    jac R;
    jac_add(&j1, &j2, &R);
    uint32_t rx[LIMBS], ry[LIMBS];
    jac_to_affine(&R, rx, ry);
    if (is_zero_n(rx, LIMBS)) return 0;  // infinity

    // rx mod n == r ?
    uint32_t rxn[LIMBS];
    if (cmp_n(rx, N, LIMBS) >= 0) mulmod(rx, one, N, MU_N, rxn);
    else memcpy(rxn, rx, sizeof(rxn));
    return cmp_n(rxn, rl, LIMBS) == 0;
}
