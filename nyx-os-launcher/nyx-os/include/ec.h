// include/ec.h — NIST P-256 (secp256r1) elliptic-curve math for the
// TLS client's ECDHE key exchange and ECDSA signature verification.
//
// Field arithmetic is Barrett-reduced 32-bit-limb bignum (little-endian
// words, 8 limbs).  Points are affine x||y (32 bytes each).  All input
// scalars and points are big-endian byte arrays, matching the TLS wire
// format.
//
// Only what TLS needs is here: scalar multiplication (ECDHE shared
// secret), an on-curve check for server public points, and ECDSA
// signature verification (used on the ServerKeyExchange).
#ifndef EC_H
#define EC_H

#include <stdint.h>

// Compute the Barrett constants for the P-256 prime and group order.
// Called once at init (idempotent).
void ec_p256_init(void);

// R = k*G, out = x(32) || y(32), big-endian. Returns 0.
void ec_p256_scalar_mult_g(const uint8_t k[32], uint8_t out[64]);

// R = k*P. P is a 64-byte affine point x||y. Returns 0 on success,
// -1 if P is not on the curve.
int ec_p256_scalar_mult(const uint8_t k[32], const uint8_t P[64],
                        uint8_t out[64]);

// 1 if P (x||y) lies on the P-256 curve, else 0.
int ec_p256_on_curve(const uint8_t P[64]);

// Verify an ECDSA-SHA256 signature (DER-encoded {r, s} SEQUENCE)
// over `digest` (the SHA-256 of the signed data) with public key Q
// (x||y, big-endian). Returns 1 on valid, 0 on invalid.
int ec_p256_verify(const uint8_t Q[64], const uint8_t digest[32],
                   const uint8_t* der_sig, int sig_len);

#endif
