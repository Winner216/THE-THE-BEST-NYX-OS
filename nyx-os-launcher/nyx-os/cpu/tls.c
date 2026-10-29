// cpu/tls.c — Minimal TLS 1.2 client.
//
// Cipher suites:
//   TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 (0xC02B) — primary, works
//     against modern servers (Wikipedia, example.com) that have
//     dropped RSA key exchange.  EC P-256 math + ECDSA verification in
//     cpu/ec.c, AES-128-GCM in cpu/aes.c.
//   TLS_RSA_WITH_AES_128_CBC_SHA (0x002F) — fallback for legacy
//     servers that still support RSA key exchange (AES-CBC + HMAC-SHA1
//     record layer).
//
//   Handshake (RFC 5246 + RFC 8422):
//     ClientHello (SNI, signature_algorithms, EC params) → ServerHello
//     → Certificate → [ServerKeyExchange] → ServerHelloDone
//     → ClientKeyExchange → ChangeCipherSpec → Finished (encrypted)
//     → server CCS + Finished (verified)
//   Records: AEAD AES-128-GCM for 0xC02B, CBC+HMAC-SHA1 for 0x002F,
//     TLS 1.2 SHA-256 PRF for master secret + key block.
//
// The X.509 certificate is parsed (DER) for its public key but never
// validated against a trust store — Nyx has no CA bundle, and TLS
// here is transport security, not identity security.
//
// All big buffers live in BSS: the kernel stack is only 16 KB and a
// 16 KB TLS record would overflow it.  The TCP transport uses the
// existing netstack (tcp_connect / tcp_send / tcp_recv — synchronous,
// whole-buffer sends).

#include <tls.h>
#include <netstack.h>
#include <aes.h>
#include <sha1.h>
#include <sha256.h>
#include <bigint.h>
#include <ec.h>
#include <io.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ─── Protocol constants ──────────────────────────────────────────
#define TLS_VER_1_2            0x0303
#define CT_CHANGE_CIPHER_SPEC  20
#define CT_ALERT               21
#define CT_HANDSHAKE           22
#define CT_APP_DATA            23

#define HS_CLIENT_HELLO         1
#define HS_SERVER_HELLO         2
#define HS_CERTIFICATE         11
#define HS_SERVER_KEY_EXCHANGE 12
#define HS_SERVER_HELLO_DONE   14
#define HS_CLIENT_KEY_EXCHANGE 16
#define HS_FINISHED            20

#define TLS_MAX_PAYLOAD      16384

// Cipher suites
#define TLS_RSA_AES128_CBC_SHA     0x002F
#define TLS_ECDHE_ECDSA_AES128_GCM 0xC02B
#define TLS_ECDHE_RSA_AES128_GCM    0xC02F

// ─── Buffers (BSS — keep off the 16 KB kernel stack) ─────────────
static uint8_t tls_rx[16384 + 4096];   // raw TCP receive stream
static int     tls_rx_len = 0;
static uint8_t tls_hs[16384 + 512];    // leftover handshake payload
static int     tls_hs_len = 0;
static uint8_t tls_out[16384 + 4096];  // record build buffer
static uint8_t tls_pt[16384 + 64];     // plaintext || MAC || padding
static uint8_t tls_app[16384];         // decrypted application data
static int     tls_app_len = 0, tls_app_pos = 0;
static uint8_t tls_msg[16384 + 512];   // one handshake message body (cert chains)


// ─── Session state ───────────────────────────────────────────────
static int      tls_state = 0;          // 0 closed, 1 handshaking, 2 open
static int      tls_suite = TLS_RSA_AES128_CBC_SHA;
static uint8_t  tls_rsa_mod[256];     // RSA modulus from server cert
static int      tls_rsa_mod_len = 0;
static uint8_t  tls_rsa_exp[4];       // RSA public exponent
static int      tls_rsa_exp_len = 0;
static uint8_t  tls_client_random[32];
static uint8_t  tls_server_random[32];
static uint8_t  tls_premaster[48];
static int      tls_premaster_len = 48;
static uint8_t  tls_master[48];
static uint8_t  tls_cw_mac[20], tls_sw_mac[20];
static uint8_t  tls_cw_key[16], tls_sw_key[16];
static uint8_t  tls_cw_iv[4], tls_sw_iv[4];   // GCM fixed IVs
static uint64_t tls_write_seq = 0, tls_read_seq = 0;
static int      tls_read_enc = 0, tls_write_enc = 0;
static sha256_ctx tls_transcript;
static char     tls_err[96];

// EC state for the ECDHE suite
static uint8_t  tls_ec_priv[32];            // client ephemeral scalar
static uint8_t  tls_ec_pub[65];             // client ephemeral point (0x04||x||y)
static uint8_t  tls_server_pub[64];         // server cert EC key (x||y)
static char     tls_sni[256];               // SNI hostname

// ─── COM1 debug (one-shot markers for the E2E harness) ───────────
static void tls_serial_char(char c) {
    int sp = 0;
    while ((inb(0x3F8 + 5) & 0x20) == 0 && sp++ < 200000) { }
    outb(0x3F8, (uint8_t)c);
}
static void tls_serial_str(const char* s) { for (; *s; s++) tls_serial_char(*s); }

static void tls_set_err(const char* s) {
    int i = 0;
    while (s[i] && i < 95) { tls_err[i] = s[i]; i++; }
    tls_err[i] = '\0';
}

const char* tls_last_error(void) { return tls_err; }
int tls_is_connected(void) { return tls_state == 2; }

// ─── Tiny PRNG (xorshift32 stirred with the PIT tick) ────────────
static uint32_t tls_rng = 0x6D2B79F5;
static uint8_t tls_rand_byte(void) {
    extern volatile uint32_t tick;
    tls_rng ^= tick;
    tls_rng ^= tls_rng << 13;
    tls_rng ^= tls_rng >> 17;
    tls_rng ^= tls_rng << 5;
    return (uint8_t)(tls_rng >> 24);
}
static uint8_t tls_rand_nz(void) {
    uint8_t b;
    do { b = tls_rand_byte(); } while (b == 0);
    return b;
}

// ─── HMAC-SHA1 / HMAC-SHA256 ─────────────────────────────────────
static void tls_hmac_sha1(const uint8_t* key, int klen,
                          const uint8_t* msg, int mlen, uint8_t out[20]) {
    uint8_t kpad[64];
    memset(kpad, 0, 64);
    if (klen > 64) { sha1_hash(key, klen, kpad); }
    else { memcpy(kpad, key, klen); }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(kpad[i] ^ 0x36);
        opad[i] = (uint8_t)(kpad[i] ^ 0x5C);
    }
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, msg, mlen);
    sha1_final(&c, out);
    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, out, 20);
    sha1_final(&c, out);
}

static void tls_hmac_sha256(const uint8_t* key, int klen,
                            const uint8_t* msg, int mlen, uint8_t out[32]) {
    uint8_t kpad[64];
    memset(kpad, 0, 64);
    if (klen > 64) { sha256_hash(key, klen, kpad); }
    else { memcpy(kpad, key, klen); }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(kpad[i] ^ 0x36);
        opad[i] = (uint8_t)(kpad[i] ^ 0x5C);
    }
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, msg, mlen);
    sha256_final(&c, out);
    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, out, 32);
    sha256_final(&c, out);
}

// ─── TLS 1.2 PRF (P_SHA256) ──────────────────────────────────────
// PRF(secret, label, seed) = P_SHA256(secret, label || seed)
static void tls_prf(const uint8_t* secret, int slen, const char* label,
                    const uint8_t* seed, int seed_len,
                    uint8_t* out, int out_len) {
    int llen = 0;
    while (label[llen]) llen++;
    uint8_t s[96];                       // label + seed (≤ 32 + 64)
    memcpy(s, label, llen);
    memcpy(s + llen, seed, seed_len);
    int sl = llen + seed_len;

    uint8_t A[32];
    tls_hmac_sha256(secret, slen, s, sl, A);       // A(1)

    int done = 0;
    while (done < out_len) {
        uint8_t in[32 + 96];
        memcpy(in, A, 32);
        memcpy(in + 32, s, sl);
        uint8_t h[32];
        tls_hmac_sha256(secret, slen, in, 32 + sl, h);
        int n = out_len - done;
        if (n > 32) n = 32;
        memcpy(out + done, h, n);
        done += n;
        if (done < out_len)
            tls_hmac_sha256(secret, slen, A, 32, A);   // A(i+1)
    }
}

// ─── Key derivation ──────────────────────────────────────────────
static void tls_derive_keys(void) {
    uint8_t seed[64];
    memcpy(seed, tls_client_random, 32);
    memcpy(seed + 32, tls_server_random, 32);
    tls_prf(tls_premaster, tls_premaster_len, "master secret",
            seed, 64, tls_master, 48);

    memcpy(seed, tls_server_random, 32);
    memcpy(seed + 32, tls_client_random, 32);
    if (tls_suite == TLS_ECDHE_ECDSA_AES128_GCM) {
        // AEAD: no MAC keys; key block = cw_key(16) sw_key(16)
        //         cw_IV(4) sw_IV(4) = 40 bytes
        uint8_t kb[40];
        tls_prf(tls_master, 48, "key expansion", seed, 64, kb, 40);
        memcpy(tls_cw_key, kb, 16);
        memcpy(tls_sw_key, kb + 16, 16);
        memcpy(tls_cw_iv, kb + 32, 4);
        memcpy(tls_sw_iv, kb + 36, 4);
    } else {
        uint8_t kb[104];
        tls_prf(tls_master, 48, "key expansion", seed, 64, kb, 104);
        memcpy(tls_cw_mac, kb, 20);          // client write MAC
        memcpy(tls_sw_mac, kb + 20, 20);     // server write MAC
        memcpy(tls_cw_key, kb + 40, 16);     // client write key
        memcpy(tls_sw_key, kb + 56, 16);     // server write key
        // kb[72..88) client IV, kb[88..104) server IV — TLS 1.1+ uses
        // per-record explicit IVs, so the implicit ones are unused.
    }
}

// ─── TCP transport helpers ───────────────────────────────────────
// Fill tls_rx until at least `need` bytes are buffered, polling the
// NIC.  Returns 1 on success, 0 on timeout or connection close.
static int tls_tcp_fill(int need, uint32_t deadline) {
    extern volatile uint32_t tick;
    while (tls_rx_len < need) {
        for (int p = 0; p < 50; p++) netstack_poll();
        uint8_t chunk[512];
        int n = tcp_recv(chunk, sizeof(chunk));
        if (n > 0) {
            for (int i = 0; i < n && tls_rx_len < (int)sizeof(tls_rx); i++)
                tls_rx[tls_rx_len++] = chunk[i];
        }
        // Re-check after draining: a server that sends data + FIN in
        // the same poll batch reaches CLOSE_WAIT below, and bailing on
        // the state would discard freshly-arrived bytes.
        if (tls_rx_len >= need) break;
        int st = tcp_get_state();
        // A server often sends the full response and the FIN together.
        // CLOSE_WAIT only becomes final once every buffered byte has
        // been drained — otherwise we'd discard the tail of a record
        // (seen as "record timeout rx=1024" for a 1720-byte record).
        if (st == TCP_CLOSE_WAIT && tcp_avail() > 0) continue;
        if (st == TCP_CLOSED || st == TCP_CLOSE_WAIT) return 0;
        if ((int)(tick - deadline) >= 0) return 0;
    }
    return 1;
}

static int tls_tcp_send_all(const uint8_t* data, int len) {
    int off = 0;
    while (off < len) {
        int n = tcp_send(data + off, len - off);
        if (n < 0) return -1;
        off += n;
        if (n == 0) {
            for (int p = 0; p < 50; p++) netstack_poll();
        }
    }
    return off;
}

// ─── Record send ─────────────────────────────────────────────────
static int tls_send_record(uint8_t type, const uint8_t* payload, int plen) {
    uint8_t* o = tls_out;

    if (!tls_write_enc) {
        o[0] = type;
        o[1] = 0x03; o[2] = 0x03;
        o[3] = (uint8_t)(plen >> 8);
        o[4] = (uint8_t)plen;
        memcpy(o + 5, payload, plen);
        if (tls_tcp_send_all(o, 5 + plen) < 0) return -1;
        return plen;
    }

    uint64_t seq = tls_write_seq;
    uint8_t seqb[8];
    for (int i = 7; i >= 0; i--) { seqb[i] = (uint8_t)(seq & 0xFF); seq >>= 8; }

    if (tls_suite == TLS_ECDHE_ECDSA_AES128_GCM) {
        // AEAD record: seq(8) || type || ver || len || explicit(8) || ct || tag
        // nonce = fixed_IV(4) || seq(8); AAD = seq || type || ver || plen.
        uint8_t nonce[12];
        memcpy(nonce, tls_cw_iv, 4);
        memcpy(nonce + 4, seqb, 8);
        uint8_t aad[13];
        memcpy(aad, seqb, 8);
        aad[8] = type; aad[9] = 0x03; aad[10] = 0x03;
        aad[11] = (uint8_t)(plen >> 8); aad[12] = (uint8_t)plen;
        uint8_t tag[16];
        aes128_gcm_seal(tls_cw_key, nonce, aad, 13, payload, plen,
                        o + 5 + 8, tag);
        int rec_len = 8 + plen + 16;
        o[0] = type;
        o[1] = 0x03; o[2] = 0x03;
        o[3] = (uint8_t)(rec_len >> 8);
        o[4] = (uint8_t)rec_len;
        memcpy(o + 5, seqb, 8);          // explicit nonce = write seq
        memcpy(o + 5 + 8 + plen, tag, 16);
        tls_write_seq++;
        if (tls_tcp_send_all(o, 5 + rec_len) < 0) return -1;
        return plen;
    }

    // CBC path (TLS_RSA_WITH_AES_128_CBC_SHA):
    // content || MAC || padding, with an explicit IV block.

    // HMAC input: seq(8) || type(1) || version(2) || length(2) || content
    uint8_t* mi = tls_pt;                       // reuse plaintext buffer
    memcpy(mi, seqb, 8);
    mi[8] = type; mi[9] = 0x03; mi[10] = 0x03;
    mi[11] = (uint8_t)(plen >> 8); mi[12] = (uint8_t)plen;
    memcpy(mi + 13, payload, plen);
    uint8_t mac[20];
    tls_hmac_sha1(tls_cw_mac, 20, mi, 13 + plen, mac);

    // plaintext = content || mac || pad
    memcpy(tls_pt, payload, plen);
    memcpy(tls_pt + plen, mac, 20);
    int content_len = plen + 20;
    int pad = 16 - (content_len % 16);
    int total = content_len + pad;              // multiple of 16
    for (int i = 0; i < pad; i++)
        tls_pt[content_len + i] = (uint8_t)(pad - 1);

    // explicit IV
    uint8_t iv[16];
    for (int i = 0; i < 16; i++) iv[i] = tls_rand_byte();

    int rec_len = 16 + total;
    o[0] = type;
    o[1] = 0x03; o[2] = 0x03;
    o[3] = (uint8_t)(rec_len >> 8);
    o[4] = (uint8_t)rec_len;
    memcpy(o + 5, iv, 16);
    aes128_ctx c;
    aes128_setkey(&c, tls_cw_key);
    aes128_cbc_encrypt(&c, tls_pt, o + 21, total, iv);

    tls_write_seq++;
    if (tls_tcp_send_all(o, 5 + rec_len) < 0) return -1;
    return plen;
}

// ─── Record receive ──────────────────────────────────────────────
// Peel one complete record from tls_rx.  If the read side is
// encrypted the payload is decrypted + MAC-verified into `out`.
// ChangeCipherSpec records are never encrypted.  On success returns
// the record type (out_len set); on failure (alert, timeout, MAC
// mismatch, bad padding) returns -1 and consumes the record.
static int tls_peel_record(uint8_t want_type, uint8_t* out, int out_max,
                           int* out_len, uint32_t deadline) {
    extern volatile uint32_t tick;
    if (!tls_tcp_fill(5, deadline)) { tls_set_err("record timeout"); return -1; }

    uint8_t type = tls_rx[0];
    int ver = (tls_rx[1] << 8) | tls_rx[2];
    int rlen = (tls_rx[3] << 8) | tls_rx[4];
    if (ver != TLS_VER_1_2) {
        // Some servers announce 1.0 for the first record; tolerate.
        if (ver != 0x0301) { tls_set_err("bad record version"); return -1; }
    }
    if (rlen < 0 || rlen > TLS_MAX_PAYLOAD + 1024) {
        tls_set_err("bad record length");
        return -1;
    }
    if (!tls_tcp_fill(5 + rlen, deadline)) { tls_set_err("record timeout"); return -1; }

    // consume the header
    memmove(tls_rx, tls_rx + 5, tls_rx_len - 5);
    tls_rx_len -= 5;

    const uint8_t* payload = tls_rx;   // record payload now at front
    int plen = rlen;

    int decrypted = 0;
    if (type == CT_ALERT) {
        // Post-handshake alert records are ENCRYPTED, so the level and
        // description bytes live inside the ciphertext — reading
        // payload[1] here would print random bytes (seen as "TLS alert
        // 132/142" for every server's close_notify).  The overwhelmingly
        // common case at this stage is the server's close_notify ending
        // the stream; consume the record and signal EOF.  Only a clear-
        // text alert (pre-ChangeCipherSpec) gets its description parsed.
        if (tls_read_enc) {
            tls_set_err("TLS close_notify");
        } else if (plen >= 2) {
            char buf[32];
            int i = 0;
            int v = payload[1];
            // "TLS alert 42"
            const char* p = "TLS alert ";
            for (; *p && i < 30; p++) buf[i++] = *p;
            if (v >= 100) buf[i++] = (char)('0' + v / 100);
            if (v >= 10)  buf[i++] = (char)('0' + (v / 10) % 10);
            buf[i++] = (char)('0' + v % 10);
            buf[i] = '\0';
            tls_set_err(buf);
        } else {
            tls_set_err("TLS alert");
        }
        memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
        tls_rx_len -= plen;
        return -1;
    }

    if (tls_read_enc && type != CT_CHANGE_CIPHER_SPEC) {
        if (tls_suite == TLS_ECDHE_ECDSA_AES128_GCM) {
            // AEAD record: explicit(8) || ct || tag; AAD over read seq.
            if (plen < 24) {
                tls_set_err("bad AEAD length");
                memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
                tls_rx_len -= plen;
                return -1;
            }
            int ct_len = plen - 24;
            if (ct_len > out_max) {
                tls_set_err("record too big for buffer");
                memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
                tls_rx_len -= plen;
                return -1;
            }
            uint8_t nonce[12];
            memcpy(nonce, tls_sw_iv, 4);
            memcpy(nonce + 4, payload, 8);   // explicit nonce
            uint64_t seq = tls_read_seq;
            uint8_t seqb[8];
            for (int i = 7; i >= 0; i--) { seqb[i] = (uint8_t)(seq & 0xFF); seq >>= 8; }
            uint8_t aad[13];
            memcpy(aad, seqb, 8);
            aad[8] = type; aad[9] = 0x03; aad[10] = 0x03;
            aad[11] = (uint8_t)(ct_len >> 8); aad[12] = (uint8_t)ct_len;
            const uint8_t* ct = payload + 8;
            const uint8_t* tag = payload + 8 + ct_len;
            if (!aes128_gcm_open(tls_sw_key, nonce, aad, 13, ct, ct_len,
                                 out, tag)) {
                tls_set_err("GCM auth failed");
                memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
                tls_rx_len -= plen;
                return -1;
            }
            tls_read_seq++;
            decrypted = 1;
            *out_len = ct_len;
        } else {
        // CBC decrypt with explicit IV
        if (plen < 32 || ((plen - 16) % 16) != 0) {
            tls_set_err("bad ciphertext length");
            memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
            tls_rx_len -= plen;
            return -1;
        }
        int ct_len = plen - 16;
        if (ct_len > out_max) {
            tls_set_err("record too big for buffer");
            memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
            tls_rx_len -= plen;
            return -1;
        }
        const uint8_t* iv = payload;
        const uint8_t* ct = payload + 16;
        aes128_ctx c;
        aes128_setkey(&c, tls_sw_key);
        aes128_cbc_decrypt(&c, ct, out, ct_len, iv);

        // strip padding
        int pt_len = ct_len;
        uint8_t pad = out[pt_len - 1];
        if (pad >= 16 || (int)pad >= pt_len) {
            tls_set_err("bad padding");
            memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
            tls_rx_len -= plen;
            return -1;
        }
        pt_len -= (pad + 1);
        if (pt_len < 21) {               // needs ≥ 1 content + 20 MAC
            tls_set_err("short plaintext");
            memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
            tls_rx_len -= plen;
            return -1;
        }
        int content_len = pt_len - 20;

        // verify MAC: HMAC(sw_mac, seq || type || ver || len || content)
        uint64_t seq = tls_read_seq;
        uint8_t seqb[8];
        for (int i = 7; i >= 0; i--) { seqb[i] = (uint8_t)(seq & 0xFF); seq >>= 8; }
        uint8_t* mi = tls_pt;
        memcpy(mi, seqb, 8);
        mi[8] = type; mi[9] = 0x03; mi[10] = 0x03;
        mi[11] = (uint8_t)(content_len >> 8); mi[12] = (uint8_t)content_len;
        memcpy(mi + 13, out, content_len);
        uint8_t mac[20];
        tls_hmac_sha1(tls_sw_mac, 20, mi, 13 + content_len, mac);
        int badmac = 0;
        for (int i = 0; i < 20; i++)
            if (mac[i] != out[content_len + i]) { badmac = 1; break; }
        if (badmac) {
            tls_set_err("MAC mismatch");
            memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
            tls_rx_len -= plen;
            return -1;
        }

        tls_read_seq++;
        decrypted = 1;
        *out_len = content_len;          // content sits at out[0..)
        }
    } else {
        if (plen > out_max) {
            tls_set_err("record too big for buffer");
            memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
            tls_rx_len -= plen;
            return -1;
        }
        memcpy(out, payload, plen);
        *out_len = plen;
    }

    memmove(tls_rx, tls_rx + plen, tls_rx_len - plen);
    tls_rx_len -= plen;

    if (want_type != 0 && type != want_type) {
        tls_set_err("unexpected record");
        return -1;
    }
    (void)decrypted;
    return type;
}

// ─── Handshake message reading ───────────────────────────────────
// Add a received handshake message to the transcript WITH its 4-byte
// header (RFC 5246: the transcript covers type + length + body).
static void tls_transcript_add_msg(int mtype, const uint8_t* body, int blen) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)mtype;
    hdr[1] = (uint8_t)(blen >> 16);
    hdr[2] = (uint8_t)(blen >> 8);
    hdr[3] = (uint8_t)blen;
    sha256_update(&tls_transcript, hdr, 4);
    sha256_update(&tls_transcript, body, blen);
}

static int tls_read_handshake_msg(uint8_t* body, int max, int* mlen,
                                  uint32_t deadline) {
    // Need at least the 4-byte message header in tls_hs.
    while (tls_hs_len < 4) {
        if (tls_hs_len + TLS_MAX_PAYLOAD + 512 > (int)sizeof(tls_hs))
            { tls_set_err("handshake overflow"); return -1; }
        int plen = 0;
        int t = tls_peel_record(CT_HANDSHAKE, tls_hs + tls_hs_len,
                                (int)sizeof(tls_hs) - tls_hs_len, &plen, deadline);
        if (t < 0) return -1;
        tls_hs_len += plen;
    }
    int mtype = tls_hs[0];
    int mlenv = ((int)tls_hs[1] << 16) | ((int)tls_hs[2] << 8) | tls_hs[3];
    while (tls_hs_len < 4 + mlenv) {
        if (tls_hs_len + TLS_MAX_PAYLOAD + 512 > (int)sizeof(tls_hs))
            { tls_set_err("handshake overflow"); return -1; }
        int plen = 0;
        int t = tls_peel_record(CT_HANDSHAKE, tls_hs + tls_hs_len,
                                (int)sizeof(tls_hs) - tls_hs_len, &plen, deadline);
        if (t < 0) return -1;
        tls_hs_len += plen;
    }
    if (mlenv > max) { tls_set_err("handshake message too big"); return -1; }
    memcpy(body, tls_hs + 4, mlenv);
    *mlen = mlenv;
    int rem = tls_hs_len - (4 + mlenv);
    memmove(tls_hs, tls_hs + 4 + mlenv, rem);
    tls_hs_len = rem;
    return mtype;
}

// ─── Handshake message send (updates the transcript) ─────────────
static int tls_send_handshake(uint8_t mtype, const uint8_t* body, int blen) {
    uint8_t hdr[4];
    hdr[0] = mtype;
    hdr[1] = (uint8_t)(blen >> 16);
    hdr[2] = (uint8_t)(blen >> 8);
    hdr[3] = (uint8_t)blen;
    sha256_update(&tls_transcript, hdr, 4);
    sha256_update(&tls_transcript, body, blen);
    // Handshake bodies we send are small (CKE ≤ 258, Finished = 12),
    // so a stack buffer is fine here.
    uint8_t buf[4 + 260];
    memcpy(buf, hdr, 4);
    memcpy(buf + 4, body, blen);
    return tls_send_record(CT_HANDSHAKE, buf, 4 + blen);
}

// ─── DER helpers ─────────────────────────────────────────────────
typedef struct { const uint8_t* p; const uint8_t* end; } tls_der;

static int der_next(tls_der* d, uint8_t* tag, const uint8_t** val, int* vlen) {
    if (d->p >= d->end) return -1;
    uint8_t t = *d->p++;
    if (d->p >= d->end) return -1;
    uint8_t lb = *d->p++;
    int len;
    if (lb < 0x80) {
        len = lb;
    } else {
        int n = lb & 0x7F;
        if (n == 0 || n > 4 || d->p + n > d->end) return -1;
        len = 0;
        for (int i = 0; i < n; i++) len = (len << 8) | *d->p++;
    }
    if (len < 0 || d->p + len > d->end) return -1;
    *tag = t;
    *val = d->p;
    *vlen = len;
    d->p += len;
    return 0;
}

// Parse the leaf X.509 certificate out of a TLS Certificate message
// (3-byte list length + [3-byte length + DER] entries) and extract
// the RSA modulus + public exponent as big-endian byte arrays.
// Returns 0 on success.
static int tls_parse_cert(const uint8_t* msg, int mlen,
                          uint8_t* n_out, int* n_len,
                          uint8_t* e_out, int* e_len) {
    if (mlen < 3) return -1;
    int list_len = ((int)msg[0] << 16) | ((int)msg[1] << 8) | msg[2];
    if (3 + list_len > mlen) return -1;
    const uint8_t* p = msg + 3;
    const uint8_t* end = p + list_len;
    if (end - p < 3) return -1;
    int clen = ((int)p[0] << 16) | ((int)p[1] << 8) | p[2];
    p += 3;
    if (p + clen > end) return -1;
    const uint8_t* cert = p;

    // Certificate ::= SEQUENCE { tbsCertificate, ... }
    tls_der d = { cert, cert + clen };
    uint8_t tag; const uint8_t* val; int vlen;
    if (der_next(&d, &tag, &val, &vlen) < 0 || tag != 0x30) return -1;
    // tbsCertificate ::= SEQUENCE
    tls_der t = { val, val + vlen };
    if (der_next(&t, &tag, &val, &vlen) < 0 || tag != 0x30) return -1;
    tls_der tbs = { val, val + vlen };
    // optional version [0] EXPLICIT
    if (der_next(&tbs, &tag, &val, &vlen) < 0) return -1;
    if (tag != 0xA0) {
        // this was the serialNumber — need 5 more fixed elements
        // (sig, issuer, validity, subject, subjectPublicKeyInfo)
    } else {
        // version consumed — serialNumber is next
        if (der_next(&tbs, &tag, &val, &vlen) < 0) return -1;
    }
    for (int i = 0; i < 4; i++) {        // sig, issuer, validity, subject
        if (der_next(&tbs, &tag, &val, &vlen) < 0) return -1;
    }
    // 6th fixed element: subjectPublicKeyInfo ::= SEQUENCE
    if (der_next(&tbs, &tag, &val, &vlen) < 0 || tag != 0x30) return -1;
    tls_der spki = { val, val + vlen };
    if (der_next(&spki, &tag, &val, &vlen) < 0 || tag != 0x30) return -1;  // alg
    if (der_next(&spki, &tag, &val, &vlen) < 0 || tag != 0x03) return -1;  // BIT STRING
    if (vlen < 4) return -1;
    const uint8_t* rpk = val + 1;        // skip unused-bits byte
    int rpk_len = vlen - 1;
    tls_der r = { rpk, rpk + rpk_len };
    if (der_next(&r, &tag, &val, &vlen) < 0 || tag != 0x30) return -1;      // RSAPublicKey
    tls_der rsa = { val, val + vlen };
    if (der_next(&rsa, &tag, &val, &vlen) < 0 || tag != 0x02) return -1;    // modulus
    if (vlen < 1 || vlen > 256) return -1;
    if (val[0] == 0x00) { val++; vlen--; }   // strip DER leading zero
    if (vlen != 128 && vlen != 256) return -1;
    memcpy(n_out, val, vlen);
    *n_len = vlen;
    if (der_next(&rsa, &tag, &val, &vlen) < 0 || tag != 0x02) return -1;    // exponent
    if (vlen > 1 && val[0] == 0x00) { val++; vlen--; }
    memcpy(e_out, val, vlen);
    *e_len = vlen;
    return 0;
}

// Parse the leaf certificate for an ECDSA P-256 public key.  Walks
// the same DER structure as tls_parse_cert but requires the SPKI
// AlgorithmIdentifier to be id-ecPublicKey (1.2.840.10045.2.1) with
// the prime256v1 curve (1.2.840.10045.3.1.7), then extracts the
// uncompressed public point into out[64] (x||y).  Returns 0 on
// success.
static int tls_parse_cert_ec(const uint8_t* msg, int mlen, uint8_t out[64]) {
    static const uint8_t oid_ec[7]  = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
    static const uint8_t oid_p256[8] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
    if (mlen < 3) return -1;
    int list_len = ((int)msg[0] << 16) | ((int)msg[1] << 8) | msg[2];
    if (3 + list_len > mlen) return -1;
    const uint8_t* p = msg + 3;
    const uint8_t* end = p + list_len;
    if (end - p < 3) return -1;
    int clen = ((int)p[0] << 16) | ((int)p[1] << 8) | p[2];
    p += 3;
    if (p + clen > end) return -1;
    const uint8_t* cert = p;

    tls_der d = { cert, cert + clen };
    uint8_t tag; const uint8_t* val; int vlen;
    if (der_next(&d, &tag, &val, &vlen) < 0 || tag != 0x30) return -1;
    tls_der t = { val, val + vlen };
    if (der_next(&t, &tag, &val, &vlen) < 0 || tag != 0x30) return -1;
    tls_der tbs = { val, val + vlen };
    if (der_next(&tbs, &tag, &val, &vlen) < 0) return -1;
    if (tag != 0xA0) {
        // this was the serialNumber — 4 fixed elements remain
        // (sig, issuer, validity, subject)
    } else {
        if (der_next(&tbs, &tag, &val, &vlen) < 0) return -1;
    }
    for (int i = 0; i < 4; i++)
        if (der_next(&tbs, &tag, &val, &vlen) < 0) return -1;
    if (der_next(&tbs, &tag, &val, &vlen) < 0 || tag != 0x30) return -1;  // spki
    tls_der spki = { val, val + vlen };
    if (der_next(&spki, &tag, &val, &vlen) < 0 || tag != 0x30) return -1;  // alg
    tls_der alg = { val, val + vlen };
    if (der_next(&alg, &tag, &val, &vlen) < 0 || tag != 0x06) return -1;
    if (vlen != 7 || memcmp(val, oid_ec, 7) != 0) return -1;
    if (der_next(&alg, &tag, &val, &vlen) < 0 || tag != 0x06) return -1;
    if (vlen != 8 || memcmp(val, oid_p256, 8) != 0) return -1;
    if (der_next(&spki, &tag, &val, &vlen) < 0 || tag != 0x03) return -1;  // BIT STRING
    if (vlen < 66) return -1;                 // unused-bits + 0x04 + 32 + 32
    const uint8_t* pt = val + 1;
    if (pt[0] != 0x04) return -1;
    memcpy(out, pt + 1, 64);
    return 0;
}

// ─── RSA public encrypt (PKCS#1 v1.5, e = 65537) ─────────────────
// out must hold at least n_len bytes; returns ciphertext length on
// success, 0 on failure.
static int tls_rsa_encrypt(const uint8_t* n_be, int n_len,
                           const uint8_t* e_be, int e_len,
                           const uint8_t* msg, int msg_len,
                           uint8_t* out, int out_max) {
    // Only the universal exponent 65537 is supported.
    int e_ok = (e_len == 3 && e_be[0] == 0x01 && e_be[1] == 0x00 && e_be[2] == 0x01) ||
               (e_len == 2 && e_be[0] == 0x01 && e_be[1] == 0x00) ||
               (e_len == 1 && e_be[0] == 0x01) ||
               (e_len == 5 && e_be[0] == 0x00 && e_be[1] == 0x01 &&
                e_be[2] == 0x00 && e_be[3] == 0x00 && e_be[4] == 0x01);
    if (!e_ok) { tls_set_err("RSA exponent not 65537"); return 0; }
    int k = n_len;
    if ((k != 128 && k != 256) || k > out_max) return 0;
    if (msg_len > k - 11) return 0;      // PKCS#1 v1.5 needs 11+ bytes

    uint8_t em[256];
    em[0] = 0x00;
    em[1] = 0x02;
    int ps = k - 3 - msg_len;
    if (ps < 8) return 0;
    for (int i = 0; i < ps; i++) em[2 + i] = tls_rand_nz();
    em[2 + ps] = 0x00;
    memcpy(em + 3 + ps, msg, msg_len);

    uint32_t nl[BI_LIMBS], bl[BI_LIMBS], rl[BI_LIMBS], el[BI_LIMBS];
    bi_from_bytes(nl, n_be, k);
    bi_from_bytes(bl, em, k);
    bi_set_word(el, 65537);
    bi_mod_pow(rl, bl, el, 17, nl);
    bi_to_bytes(rl, out, k);
    return k;
}


// ─── RSA-PKCS1-v1.5 signature verification ──────────────────────
// Verifies an RSA signature over a SHA-256 digest.
static int tls_rsa_verify_sig(const uint8_t* n_be, int n_len,
                               const uint8_t* e_be, int e_len,
                               const uint8_t digest[32],
                               const uint8_t* sig, int sig_len) {
    if (n_len != 128 && n_len != 256) return 0;
    if (sig_len > n_len) sig_len = n_len;
    uint32_t nl[BI_LIMBS], sl[BI_LIMBS], rl[BI_LIMBS], el[BI_LIMBS];
    bi_from_bytes(nl, n_be, n_len);
    bi_from_bytes(sl, sig, sig_len);
    uint32_t eval = 0;
    for (int i = 0; i < e_len; i++) eval = (eval << 8) | e_be[i];
    bi_set_word(el, eval);
    bi_mod_pow(rl, sl, el, 17, nl);
    uint8_t dec[256];
    bi_to_bytes(rl, dec, n_len);
    if (dec[0] != 0x00 || dec[1] != 0x01) return 0;
    int i = 2;
    while (i < n_len && dec[i] == 0xFF) i++;
    if (i >= n_len || dec[i] != 0x00) return 0;
    i++;
    static const uint8_t sha256_di[] = {
        0x30,0x31,0x30,0x0D,0x06,0x09,0x60,0x86,0x48,0x01,
        0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
    };
    if (i + 51 > n_len) return 0;
    if (memcmp(dec + i, sha256_di, 19) != 0) return 0;
    if (memcmp(dec + i + 19, digest, 32) != 0) return 0;
    return 1;
}

// ─── The handshake ───────────────────────────────────────────────
int tls_connect(uint32_t ip, uint16_t port, const char* host) {
    extern volatile uint32_t tick;
    uint32_t deadline = tick + 600;      // ~12 s at 50 Hz

    tls_state = 0;
    tls_sni[0] = '\0';
    if (host) {
        int i = 0;
        while (host[i] && i < 254) { tls_sni[i] = host[i]; i++; }
        tls_sni[i] = '\0';
    }
    tls_read_enc = 0;
    tls_write_enc = 0;
    tls_write_seq = 0;
    tls_read_seq = 0;
    tls_rx_len = 0;
    tls_hs_len = 0;
    tls_app_len = 0;
    tls_app_pos = 0;
    tls_err[0] = '\0';

    if (tcp_connect(ip, port) < 0) {
        tls_set_err("TCP connect failed");
        return -1;
    }

    sha256_init(&tls_transcript);

    // ── ClientHello ──
    for (int i = 0; i < 32; i++) tls_client_random[i] = tls_rand_byte();
    uint8_t ch[512];
    int p = 0;
    ch[p++] = 0x03; ch[p++] = 0x03;
    memcpy(ch + p, tls_client_random, 32); p += 32;
    ch[p++] = 0;                         // session id length
    ch[p++] = 0x00; ch[p++] = 0x06;      // cipher suites length (3 × 2 bytes)
    ch[p++] = 0xC0; ch[p++] = 0x2F;      // ECDHE_RSA_AES128_GCM_SHA256 (YouTube, Google)
    ch[p++] = 0xC0; ch[p++] = 0x2B;      // ECDHE_ECDSA_AES128_GCM_SHA256 (GitHub)
    ch[p++] = 0x00; ch[p++] = 0x2F;      // RSA_AES128_CBC_SHA (legacy fallback)
    ch[p++] = 0x01;                      // compression methods length
    ch[p++] = 0x00;                      // null

    // Extensions.  Layout: 2-byte total len, then (type, len, data) per ext.
    int ext_start = p;
    ch[p++] = 0x00; ch[p++] = 0x00;      // total length placeholder

    // SNI (0x0000) — critical for virtual-host servers (wikipedia).
    if (tls_sni[0]) {
        int nl = 0;
        while (tls_sni[nl] && nl < 255) nl++;
        ch[p++] = 0x00; ch[p++] = 0x00;  // type
        int elen = 5 + nl;               // list(2) + type(1) + namelen(2) + name
        ch[p++] = (uint8_t)(elen >> 8); ch[p++] = (uint8_t)elen;
        ch[p++] = 0x00; ch[p++] = (uint8_t)(nl + 3);
        ch[p++] = 0x00;                  // host_name
        ch[p++] = (uint8_t)(nl >> 8); ch[p++] = (uint8_t)nl;
        memcpy(ch + p, tls_sni, nl); p += nl;
    }
    // signature_algorithms (0x000D): ext = list-len(2) + 2 pairs
    ch[p++] = 0x00; ch[p++] = 0x0D;
    ch[p++] = 0x00; ch[p++] = 0x06;      // extension data length (6)
    ch[p++] = 0x00; ch[p++] = 0x04;      // list length (2 pairs × 2 bytes)
    ch[p++] = 0x04; ch[p++] = 0x03;      // SHA-256 + ECDSA
    ch[p++] = 0x04; ch[p++] = 0x01;      // SHA-256 + RSA
    // supported_groups (0x000A): ext = list-len(2) + secp256r1
    ch[p++] = 0x00; ch[p++] = 0x0A;
    ch[p++] = 0x00; ch[p++] = 0x04;      // extension data length (4)
    ch[p++] = 0x00; ch[p++] = 0x02;      // list length (one 2-byte group)
    ch[p++] = 0x00; ch[p++] = 0x17;      // secp256r1
    // ec_point_formats (0x000B): uncompressed
    ch[p++] = 0x00; ch[p++] = 0x0B;
    ch[p++] = 0x00; ch[p++] = 0x02;      // list len
    ch[p++] = 0x01;                      // formats count
    ch[p++] = 0x00;                      // uncompressed
    int ext_len = p - (ext_start + 2);
    ch[ext_start] = (uint8_t)(ext_len >> 8);
    ch[ext_start + 1] = (uint8_t)ext_len;

    if (tls_send_handshake(HS_CLIENT_HELLO, ch, p) < 0) {
        tls_set_err("send ClientHello failed");
        tcp_close();
        return -1;
    }

    // ── ServerHello ──
    uint8_t* body = tls_msg;             // BSS — cert chains can be KBs
    int blen = 0;
    int    mt = tls_read_handshake_msg(body, (int)sizeof(tls_msg), &blen, deadline);
    if (mt != HS_SERVER_HELLO) {
        if (!tls_err[0]) tls_set_err("no ServerHello");
        tcp_close();
        return -1;
    }
    if (blen < 38) { tls_set_err("short ServerHello"); tcp_close(); return -1; }
    if (body[0] != 0x03 || body[1] != 0x03) {
        tls_set_err("server is not TLS 1.2");
        tcp_close();
        return -1;
    }
    memcpy(tls_server_random, body + 2, 32);
    int sid_len = body[34];
    if (35 + sid_len + 2 > blen) { tls_set_err("bad ServerHello"); tcp_close(); return -1; }
    int cs = ((int)body[35 + sid_len] << 8) | body[36 + sid_len];
    if (cs == 0x002F) {
        tls_suite = TLS_RSA_AES128_CBC_SHA;
    } else if (cs == TLS_ECDHE_ECDSA_AES128_GCM) {
        tls_suite = TLS_ECDHE_ECDSA_AES128_GCM;
        // Client ephemeral ECDHE key: random scalar, pub = k*G.
        for (int i = 0; i < 32; i++) tls_ec_priv[i] = tls_rand_byte();
        uint8_t pt[64];
        ec_p256_scalar_mult_g(tls_ec_priv, pt);
        tls_ec_pub[0] = 0x04;
        memcpy(tls_ec_pub + 1, pt, 64);
    } else if (cs == TLS_ECDHE_RSA_AES128_GCM) {
        tls_suite = TLS_ECDHE_RSA_AES128_GCM;
        // Client ephemeral ECDHE key (same as ECDSA path)
        for (int i = 0; i < 32; i++) tls_ec_priv[i] = tls_rand_byte();
        uint8_t pt[64];
        ec_p256_scalar_mult_g(tls_ec_priv, pt);
        tls_ec_pub[0] = 0x04;
        memcpy(tls_ec_pub + 1, pt, 64);
    } else {
        tls_set_err("server cipher not supported");
        tcp_close();
        return -1;
    }
    tls_transcript_add_msg(HS_SERVER_HELLO, body, blen);

    // ── Certificate (suite-aware: EC cert for ECDHE, RSA for fallback) ──
    mt = tls_read_handshake_msg(body, (int)sizeof(tls_msg), &blen, deadline);
    if (mt != HS_CERTIFICATE) { tls_set_err("no certificate"); tcp_close(); return -1; }
    uint8_t n_be[256], e_be[8];
    int n_len = 0, e_len = 0;
    if (tls_suite == TLS_ECDHE_ECDSA_AES128_GCM) {
        if (tls_parse_cert_ec(body, blen, tls_server_pub) < 0) {
            tls_set_err("EC certificate parse failed");
            tcp_close();
            return -1;
        }
    } else {
        // RSA cert for both RSA_AES128_CBC_SHA and ECDHE_RSA_AES128_GCM
        if (tls_parse_cert(body, blen, n_be, &n_len, e_be, &e_len) < 0) {
            tls_set_err("certificate parse failed");
            tcp_close();
            return -1;
        }
        // Cache RSA key for signature verification in ECDHE_RSA path
        memcpy(tls_rsa_mod, n_be, n_len);
        tls_rsa_mod_len = n_len;
        memcpy(tls_rsa_exp, e_be, e_len);
        tls_rsa_exp_len = e_len;
    }
    tls_transcript_add_msg(HS_CERTIFICATE, body, blen);

    // ── ServerKeyExchange (ECDHE only): params + ECDSA/RSA signature ──
    uint8_t server_eph[64];               // server ephemeral point (x||y)
    int have_server_eph = 0;
    if (tls_suite == TLS_ECDHE_ECDSA_AES128_GCM ||
        tls_suite == TLS_ECDHE_RSA_AES128_GCM) {
        mt = tls_read_handshake_msg(body, (int)sizeof(tls_msg), &blen, deadline);
        if (mt != HS_SERVER_KEY_EXCHANGE) {
            tls_set_err("no ServerKeyExchange");
            tcp_close();
            return -1;
        }
        // ServerECDHParams: curve_type(1)=3, namedcurve(2)=0x0017,
        // point_len(1), point(point_len).  Then TLS 1.2
        // SignatureAndHashAlgorithm(2) + sig_len(2) + DER sig.
        if (blen < 6) { tls_set_err("short SKE"); tcp_close(); return -1; }
        int o = 0;
        if (body[o++] != 0x03) { tls_set_err("SKE curve type"); tcp_close(); return -1; }
        if (body[o] != 0x00 || body[o + 1] != 0x17) {
            tls_set_err("SKE not P-256");
            tcp_close();
            return -1;
        }
        o += 2;
        int pt_len = body[o++];
        if (pt_len != 65 || body[o] != 0x04 || o + pt_len + 4 > blen) {
            tls_set_err("SKE bad point");
            tcp_close();
            return -1;
        }
        memcpy(server_eph, body + o + 1, 64);
        have_server_eph = 1;
        o += pt_len;
        // signature: hash(1) sig(1) len(2) DER
        int params_end = o;               // end of params (sig excluded)
        if (o + 4 > blen) { tls_set_err("SKE no signature"); tcp_close(); return -1; }
        int sig_len = ((int)body[o + 2] << 8) | body[o + 3];
        o += 4;
        if (o + sig_len > blen) { tls_set_err("SKE sig length"); tcp_close(); return -1; }

        // RFC 5246 7.4.3: the ECDHE ServerKeyExchange signature
        // covers SHA-256(ClientHello.random || ServerHello.random ||
        // params) — the two randoms + the SKE parameters, NOT the
        // full handshake transcript (that is the Finished's job).
        uint8_t digest[32];
        sha256_ctx d;
        sha256_init(&d);
        sha256_update(&d, tls_client_random, 32);
        sha256_update(&d, tls_server_random, 32);
        sha256_update(&d, body, params_end);
        sha256_final(&d, digest);
        // Verify the signature: ECDSA for EC certs, RSA for RSA certs
        if (tls_suite == TLS_ECDHE_ECDSA_AES128_GCM) {
            if (!ec_p256_verify(tls_server_pub, digest, body + o, sig_len)) {
                tls_set_err("SKE ECDSA signature invalid");
                tcp_close();
                return -1;
            }
        } else {
            // RSA-PKCS1-v1.5 signature verification
            // body[o] is the DER-encoded RSA signature
            if (!tls_rsa_verify_sig(tls_rsa_mod, tls_rsa_mod_len,
                                    tls_rsa_exp, tls_rsa_exp_len,
                                    digest, body + o, sig_len)) {
                tls_set_err("SKE RSA signature invalid");
                tcp_close();
                return -1;
            }
        }
        tls_transcript_add_msg(HS_SERVER_KEY_EXCHANGE, body, blen);
    }

    // ── ServerHelloDone ──
    mt = tls_read_handshake_msg(body, (int)sizeof(tls_msg), &blen, deadline);
    if (mt != HS_SERVER_HELLO_DONE) {
        tls_set_err("no ServerHelloDone");
        tcp_close();
        return -1;
    }
    tls_transcript_add_msg(HS_SERVER_HELLO_DONE, body, blen);

    // ── ClientKeyExchange + premaster ──
    uint8_t cke[2 + 256];
    int cke_len = 0;
    if (tls_suite == TLS_ECDHE_ECDSA_AES128_GCM ||
        tls_suite == TLS_ECDHE_RSA_AES128_GCM) {
        // premaster = x-coordinate of (client_priv * server_eph)
        uint8_t shared[64];
        if (ec_p256_scalar_mult(tls_ec_priv, server_eph, shared) < 0 ||
            !have_server_eph) {
            tls_set_err("ECDHE shared secret failed");
            tcp_close();
            return -1;
        }
        memcpy(tls_premaster, shared, 32);
        tls_premaster_len = 32;
        // CKE = point<1..2^8-1>: 1-byte len + uncompressed pub point
        cke[0] = 65;
        memcpy(cke + 1, tls_ec_pub, 65);
        cke_len = 1 + 65;
    } else {
        // RSA-encrypt a 48-byte premaster (2 bytes version + 46 random)
        uint8_t prem[48], enc[256];
        prem[0] = 0x03; prem[1] = 0x03;
        for (int i = 2; i < 48; i++) prem[i] = tls_rand_byte();
        memcpy(tls_premaster, prem, 48);
        tls_premaster_len = 48;
        int k = tls_rsa_encrypt(n_be, n_len, e_be, e_len, prem, 48, enc, sizeof(enc));
        if (k <= 0) { tcp_close(); return -1; }
        cke[0] = (uint8_t)(k >> 8);
        cke[1] = (uint8_t)k;
        memcpy(cke + 2, enc, k);
        cke_len = 2 + k;
    }
    if (tls_send_handshake(HS_CLIENT_KEY_EXCHANGE, cke, cke_len) < 0) {
        tls_set_err("send ClientKeyExchange failed");
        tcp_close();
        return -1;
    }
    tls_derive_keys();

    // ── ChangeCipherSpec (plaintext) ──
    uint8_t ccs[1] = { 1 };
    if (tls_send_record(CT_CHANGE_CIPHER_SPEC, ccs, 1) < 0) {
        tls_set_err("send CCS failed");
        tcp_close();
        return -1;
    }
    tls_write_enc = 1;

    // ── client Finished (encrypted) ──
    uint8_t h[32];
    sha256_ctx tc = tls_transcript;
    sha256_final(&tc, h);
    uint8_t vd[12];
    tls_prf(tls_master, 48, "client finished", h, 32, vd, 12);
    if (tls_send_handshake(HS_FINISHED, vd, 12) < 0) {
        tls_set_err("send Finished failed");
        tcp_close();
        return -1;
    }

    // ── server ChangeCipherSpec (plaintext) ──
    {
        uint8_t pl[64];
        int plen = 0;
        if (tls_peel_record(CT_CHANGE_CIPHER_SPEC, pl, sizeof(pl), &plen, deadline) < 0 ||
            plen != 1 || pl[0] != 1) {
            if (!tls_err[0]) tls_set_err("no server CCS");
            tcp_close();
            return -1;
        }
    }
    tls_read_enc = 1;

    // ── server Finished (encrypted) — verify against the transcript ──
    mt = tls_read_handshake_msg(body, (int)sizeof(tls_msg), &blen, deadline);
    if (mt != HS_FINISHED) {
        if (!tls_err[0]) tls_set_err("no server Finished");
        tcp_close();
        return -1;
    }
    if (blen < 12) { tls_set_err("short Finished"); tcp_close(); return -1; }
    sha256_ctx tc2 = tls_transcript;
    sha256_final(&tc2, h);
    uint8_t evd[12];
    tls_prf(tls_master, 48, "server finished", h, 32, evd, 12);
    if (memcmp(evd, body, 12) != 0) {
        tls_set_err("server Finished mismatch");
        tcp_close();
        return -1;
    }

    tls_state = 2;
    tls_serial_str("TLS OK\n");
    return 0;
}

// ─── Application data ────────────────────────────────────────────
int tls_send(const void* data, int len) {
    if (tls_state != 2) return -1;
    return tls_send_record(CT_APP_DATA, (const uint8_t*)data, len);
}

int tls_recv(void* buf, int max_len) {
    if (tls_state != 2) return -1;
    if (tls_app_pos < tls_app_len) {
        int n = tls_app_len - tls_app_pos;
        if (n > max_len) n = max_len;
        memcpy(buf, tls_app + tls_app_pos, n);
        tls_app_pos += n;
        return n;
    }
    extern volatile uint32_t tick;
    uint32_t deadline = tick + 400;      // ~8 s
    for (;;) {
        int plen = 0;
        int t = tls_peel_record(0, tls_app, (int)sizeof(tls_app), &plen, deadline);
        if (t < 0) {
            return -1;
        }
        if (t == CT_APP_DATA) {
            tls_app_len = plen;
            tls_app_pos = 0;
            int n = plen;
            if (n > max_len) n = max_len;
            memcpy(buf, tls_app, n);
            tls_app_pos = n;
            return n;
        }
        if (t == CT_HANDSHAKE) continue;   // NewSessionTicket etc.
        if (t == CT_CHANGE_CIPHER_SPEC) continue;
    }
}

void tls_close(void) {
    tcp_close();
    tls_state = 0;
    tls_read_enc = 0;
    tls_write_enc = 0;
}
