// cpu/openssl.c  –  Minimal OpenSSL compatibility shim for Nyx OS
#include <openssl.h>
#include <tls.h>
#include <sha256.h>
#include <sha1.h>
#include <aes.h>
#include <ec.h>
#include <bigint.h>
#include <heap.h>
#include <string.h>
#include <string.h>
#include <stdint.h>

// Simple type definitions
typedef struct { int dummy; } ssl_ctx_t;
typedef struct { int dummy; } ssl_t;
typedef struct { int dummy; } x509_t;
typedef struct { int dummy; } evp_md_t;
typedef struct { int dummy; } evp_md_ctx_t;
typedef struct { int dummy; } evp_cipher_t;
typedef struct { int dummy; } evp_cipher_ctx_t;
typedef struct { int dummy; } bio_t;
typedef struct { int dummy; } bn_t;
typedef struct { int dummy; } rsa_t;
typedef struct { int dummy; } ec_key_t;
typedef struct { int dummy; } ec_group_t;
typedef struct { int dummy; } ec_point_t;

// SSL Method
const void* TLS_client_method(void) { return NULL; }

// SSL Context
void* SSL_CTX_new(const void* m) { (void)m; ssl_ctx_t* ctx = kmalloc(sizeof(ssl_ctx_t)); if(ctx) ctx->dummy=0; return ctx; }
void SSL_CTX_free(void* c) { if(c) kfree(c); }
int SSL_CTX_set_cipher_list(void* c, const char* s) { (void)c;(void)s; return 1; }
int SSL_CTX_load_verify_locations(void* c, const char* f, const char* p) { (void)c;(void)f;(void)p; return 1; }
int SSL_CTX_set_default_verify_paths(void* c) { (void)c; return 1; }
void SSL_CTX_set_verify(void* c, int m, void* cb) { (void)c;(void)m;(void)cb; }
void SSL_CTX_set_verify_depth(void* c, int d) { (void)c;(void)d; }

// SSL Session
void* SSL_new(void* c) { (void)c; ssl_t* s = kmalloc(sizeof(ssl_t)); if(s) s->dummy=0; return s; }
void SSL_free(void* s) { if(s) kfree(s); }
int SSL_set_fd(void* s, int fd) { (void)s;(void)fd; return 1; }
int SSL_connect(void* s) { (void)s; return 1; }
int SSL_read(void* s, void* buf, int num) { (void)s; return tls_recv(buf, num); }
int SSL_write(void* s, const void* buf, int num) { (void)s; return tls_send(buf, num); }
int SSL_shutdown(void* s) { (void)s; tls_close(); return 1; }
int SSL_get_error(void* s, int r) { (void)s; return (r>0)?0:2; }
void* SSL_get_peer_certificate(void* s) { (void)s; return NULL; }
long SSL_get_verify_result(void* s) { (void)s; return 0; }
int SSL_set_tlsext_host_name(void* s, const char* n) { (void)s;(void)n; return 1; }

// X509
void X509_free(void* a) { if(a) kfree(a); }

// Error handling
unsigned long ERR_get_error(void) { return 0; }
char* ERR_error_string(unsigned long e, char* buf) { (void)e; if(!buf){static char b[64];buf=b;} strcpy(buf,"no error"); return buf; }
void ERR_free_strings(void) {}

// BIO
void* BIO_new_mem_buf(const void* buf, int len) { (void)buf;(void)len; return NULL; }
int BIO_read(void* b, void* d, int l) { (void)b;(void)d;(void)l; return 0; }
int BIO_write(void* b, const void* d, int l) { (void)b;(void)d;(void)l; return l; }
void BIO_free(void* a) { if(a) kfree(a); }

// EVP - Digest
const void* EVP_sha256(void) { static int md=0; return &md; }
const void* EVP_sha1(void) { static int md=1; return &md; }
const void* EVP_md5(void) { static int md=2; return &md; }

void* EVP_MD_CTX_new(void) { evp_md_ctx_t* c = kmalloc(sizeof(evp_md_ctx_t)); if(c) c->dummy=0; return c; }
void EVP_MD_CTX_free(void* c) { if(c) kfree(c); }
int EVP_DigestInit(void* c, const void* t) { (void)c;(void)t; return 1; }
int EVP_DigestUpdate(void* c, const void* d, size_t l) { (void)c;(void)d;(void)l; return 1; }
int EVP_DigestFinal(void* c, unsigned char* md, unsigned int* s) { (void)c; if(md) memset(md,0,32); if(s)*s=32; return 1; }

// EVP - Cipher
const void* EVP_aes_128_cbc(void) { static int c=0; return &c; }
const void* EVP_aes_256_cbc(void) { static int c=1; return &c; }

void* EVP_CIPHER_CTX_new(void) { evp_cipher_ctx_t* c = kmalloc(sizeof(evp_cipher_ctx_t)); if(c) c->dummy=0; return c; }
void EVP_CIPHER_CTX_free(void* c) { if(c) kfree(c); }
int EVP_EncryptInit(void* c, const void* t, const unsigned char* k, const unsigned char* iv) { (void)c;(void)t;(void)k;(void)iv; return 1; }
int EVP_EncryptUpdate(void* c, unsigned char* o, int* l, const unsigned char* i, int n) { (void)c; *l=n; return 1; }
int EVP_EncryptFinal(void* c, unsigned char* o, int* l) { (void)c; *l=0; return 1; }
int EVP_DecryptInit(void* c, const void* t, const unsigned char* k, const unsigned char* iv) { (void)c;(void)t;(void)k;(void)iv; return 1; }
int EVP_DecryptUpdate(void* c, unsigned char* o, int* l, const unsigned char* i, int n) { (void)c; *l=n; return 1; }
int EVP_DecryptFinal(void* c, unsigned char* o, int* l) { (void)c; *l=0; return 1; }

// HMAC
void* HMAC(const void* t, const void* k, int kl, const unsigned char* d, size_t dl, unsigned char* md, unsigned int* ml) { (void)t;(void)k;(void)kl;(void)d;(void)dl; if(ml)*ml=32; return md; }

// BIGNUM
void* BN_new(void) { bn_t* bn = kmalloc(sizeof(bn_t)); if(bn) bn->dummy=0; return bn; }
void BN_free(void* a) { if(a) kfree(a); }
int BN_bin2bn(const unsigned char* s, int l, void* r) { (void)s;(void)l;(void)r; return 1; }
int BN_bn2bin(const void* a, unsigned char* t) { (void)a; memset(t,0,256); return 256; }
int BN_num_bytes(const void* a) { (void)a; return 256; }
int BN_cmp(const void* a, const void* b) { (void)a;(void)b; return 0; }
int BN_add(void* r, const void* a, const void* b) { (void)r;(void)a;(void)b; return 1; }
int BN_sub(void* r, const void* a, const void* b) { (void)r;(void)a;(void)b; return 1; }
int BN_mul(void* r, const void* a, const void* b, void* c) { (void)r;(void)a;(void)b;(void)c; return 1; }
int BN_mod_exp(void* r, const void* a, const void* p, const void* m, void* c) { (void)r;(void)a;(void)p;(void)m;(void)c; return 1; }
int BN_mod_inverse(void* r, const void* a, const void* n, void* c) { (void)r;(void)a;(void)n;(void)c; return 1; }

// RSA
void* RSA_new(void) { rsa_t* r = kmalloc(sizeof(rsa_t)); if(r) r->dummy=0; return r; }
void RSA_free(void* r) { if(r) kfree(r); }
int RSA_generate_key_ex(void* r, int b, void* e, void* c) { (void)r;(void)b;(void)e;(void)c; return 1; }
int RSA_sign(int t, const unsigned char* m, unsigned int ml, unsigned char* s, unsigned int* sl, void* r) { (void)t;(void)m;(void)ml;(void)s;(void)sl;(void)r; return 1; }
int RSA_verify(int t, const unsigned char* m, unsigned int ml, const unsigned char* s, unsigned int sl, void* r) { (void)t;(void)m;(void)ml;(void)s;(void)sl;(void)r; return 1; }
int RSA_public_encrypt(int f, const unsigned char* f2, unsigned char* t, void* r, int p) { (void)f;(void)f2;(void)t;(void)r;(void)p; return f; }
int RSA_private_decrypt(int f, const unsigned char* f2, unsigned char* t, void* r, int p) { (void)f;(void)f2;(void)t;(void)r;(void)p; return f; }

// EC
void* EC_KEY_new_by_curve_name(int n) { (void)n; ec_key_t* k = kmalloc(sizeof(ec_key_t)); if(k) k->dummy=0; return k; }
void EC_KEY_free(void* k) { if(k) kfree(k); }
int EC_KEY_set_public_key(void* k, const void* p) { (void)k;(void)p; return 1; }
const void* EC_KEY_get0_public_key(const void* k) { (void)k; return NULL; }
void* EC_GROUP_new_by_curve_name(int n) { (void)n; return NULL; }
void* EC_POINT_new(const void* g) { (void)g; return NULL; }
void EC_POINT_free(void* p) { if(p) kfree(p); }
int EC_POINT_mul(const void* g, void* r, const void* n, const void* m, void* c) { (void)g;(void)r;(void)n;(void)m;(void)c; return 1; }
