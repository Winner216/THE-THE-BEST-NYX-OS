// include/bigint.h — 2048-bit unsigned big integers (Montgomery math)
#ifndef BIGINT_H
#define BIGINT_H

#include <stdint.h>

// 64 limbs × 32 bits = 2048 bits — enough for RSA-2048 public keys
// (used by real sites like example.com).
#define BI_LIMBS 64

// Convert big-endian bytes to limbs (len can be < 128; high limbs = 0).
void bi_from_bytes(uint32_t out[BI_LIMBS], const uint8_t* be, int len);

// Convert limbs to big-endian bytes. out must hold `len` bytes; the
// value is right-aligned (leading zero bytes are written).
void bi_to_bytes(const uint32_t in[BI_LIMBS], uint8_t* out, int len);

void bi_set_word(uint32_t a[BI_LIMBS], uint32_t v);
void bi_copy(uint32_t d[BI_LIMBS], const uint32_t s[BI_LIMBS]);
int  bi_is_zero(const uint32_t a[BI_LIMBS]);
int  bi_cmp(const uint32_t a[BI_LIMBS], const uint32_t b[BI_LIMBS]);

// a -= b (a must be >= b)
void bi_sub(uint32_t a[BI_LIMBS], const uint32_t b[BI_LIMBS]);

// out = base^exp mod n  (base, exp, n: BI_LIMBS-limb values)
// exp_bits is the bit length of exp (e.g. 17 for 65537).
void bi_mod_pow(uint32_t out[BI_LIMBS], const uint32_t base[BI_LIMBS],
                const uint32_t exp[BI_LIMBS], int exp_bits,
                const uint32_t n[BI_LIMBS]);

#endif
