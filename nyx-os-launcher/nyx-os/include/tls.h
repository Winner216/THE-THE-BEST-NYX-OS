// include/tls.h — Minimal TLS 1.2 client
#ifndef TLS_H
#define TLS_H

#include <stdint.h>

// Cipher suites:
//   TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 (0xC02B) — primary, works
//     against modern servers (wikipedia, example.com) that have dropped
//     RSA key exchange.  EC P-256 math + ECDSA verification in
//     cpu/ec.c, AES-128-GCM in cpu/aes.c.
//   TLS_RSA_WITH_AES_128_CBC_SHA (0x002F) — fallback for legacy
//     servers that still offer RSA key exchange.
//
//   Handshake: ClientHello (SNI, signature_algorithms, EC params)
//              → ServerHello → Certificate → [ServerKeyExchange]
//              → ServerHelloDone → ClientKeyExchange → CCS → Finished
//   Records:   AES-128-GCM (0xC02B) / AES-128-CBC+HMAC-SHA1 (0x002F),
//              SHA-256 PRF for key derivation.
//
// The server certificate is parsed for its public key (DER) but NOT
// validated against a trust store — this OS has no CA bundle.

// Open a TLS session to ip:port (ip in the codebase's LE wire format,
// e.g. host 10.0.2.2 → 0x0202000A).  `host` is sent as the SNI
// server name (may be NULL).  Blocks through the handshake.
// Returns 0 on success, -1 on failure (tls_last_error() explains).
int tls_connect(uint32_t ip, uint16_t port, const char* host);

// Send plaintext application data over the encrypted channel.
// Returns bytes sent or -1.
int tls_send(const void* data, int len);

// Receive plaintext application data (decrypted from records).
// Returns bytes received (0 if none yet), or -1 on error/close.
int tls_recv(void* buf, int max_len);

// Close the TLS session and its TCP connection.
void tls_close(void);

// 1 while a TLS session is established, 0 otherwise.
int tls_is_connected(void);

// Human-readable reason for the last failure (status-bar friendly).
const char* tls_last_error(void);

#endif
