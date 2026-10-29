// include/openssl.h  –  Minimal OpenSSL compatibility for Nyx OS
#ifndef OPENSSL_H
#define OPENSSL_H
#include <stdint.h>
#include <stddef.h>

// All types are opaque void* for simplicity
typedef void SSL_CTX;
typedef void SSL;
typedef void X509;
typedef void EVP_MD;
typedef void EVP_MD_CTX;
typedef void EVP_CIPHER;
typedef void EVP_CIPHER_CTX;
typedef void BIO;
typedef void BIGNUM;
typedef void RSA;
typedef void EC_KEY;
typedef void EC_GROUP;
typedef void EC_POINT;
typedef void EVP_PKEY;

// Return codes
#define SSL_SUCCESS 1
#define SSL_FAILURE 0

// SSL functions
const void* TLS_client_method(void);
void* SSL_CTX_new(const void* method);
void SSL_CTX_free(void* ctx);
int SSL_CTX_set_cipher_list(void* ctx, const char* str);
int SSL_CTX_load_verify_locations(void* ctx, const char* CAfile, const char* CApath);
int SSL_CTX_set_default_verify_paths(void* ctx);
void SSL_CTX_set_verify(void* ctx, int mode, void* callback);
void SSL_CTX_set_verify_depth(void* ctx, int depth);
void* SSL_new(void* ctx);
void SSL_free(void* ssl);
int SSL_set_fd(void* ssl, int fd);
int SSL_connect(void* ssl);
int SSL_read(void* ssl, void* buf, int num);
int SSL_write(void* ssl, const void* buf, int num);
int SSL_shutdown(void* ssl);
int SSL_get_error(void* ssl, int ret);
void* SSL_get_peer_certificate(void* ssl);
long SSL_get_verify_result(void* ssl);
int SSL_set_tlsext_host_name(void* ssl, const char* name);
void X509_free(void* a);

// Error handling
unsigned long ERR_get_error(void);
char* ERR_error_string(unsigned long e, char* buf);
void ERR_free_strings(void);

// BIO
void* BIO_new_mem_buf(const void* buf, int len);
int BIO_read(void* b, void* data, int len);
int BIO_write(void* b, const void* data, int len);
void BIO_free(void* a);

// EVP
const void* EVP_sha256(void);
const void* EVP_sha1(void);
const void* EVP_md5(void);
const void* EVP_aes_128_cbc(void);
const void* EVP_aes_256_cbc(void);
void* EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(void* ctx);
int EVP_DigestInit(void* ctx, const void* type);
int EVP_DigestUpdate(void* ctx, const void* d, size_t cnt);
int EVP_DigestFinal(void* ctx, unsigned char* md, unsigned int* s);
void* EVP_CIPHER_CTX_new(void);
void EVP_CIPHER_CTX_free(void* ctx);
int EVP_EncryptInit(void* ctx, const void* type, const unsigned char* key, const unsigned char* iv);
int EVP_EncryptUpdate(void* ctx, unsigned char* out, int* outl, const unsigned char* in, int inl);
int EVP_EncryptFinal(void* ctx, unsigned char* out, int* outl);
int EVP_DecryptInit(void* ctx, const void* type, const unsigned char* key, const unsigned char* iv);
int EVP_DecryptUpdate(void* ctx, unsigned char* out, int* outl, const unsigned char* in, int inl);
int EVP_DecryptFinal(void* ctx, unsigned char* out, int* outl);

// HMAC
void* HMAC(const void* evp_md, const void* key, int key_len, const unsigned char* data, size_t data_len, unsigned char* md, unsigned int* md_len);

// BIGNUM
void* BN_new(void);
void BN_free(void* a);
int BN_bin2bn(const unsigned char* s, int len, void* ret);
int BN_bn2bin(const void* a, unsigned char* to);
int BN_num_bytes(const void* a);
int BN_cmp(const void* a, const void* b);
int BN_add(void* r, const void* a, const void* b);
int BN_sub(void* r, const void* a, const void* b);
int BN_mul(void* r, const void* a, const void* b, void* ctx);
int BN_mod_exp(void* r, const void* a, const void* p, const void* m, void* ctx);
int BN_mod_inverse(void* ret, const void* a, const void* n, void* ctx);

// RSA
void* RSA_new(void);
void RSA_free(void* rsa);
int RSA_generate_key_ex(void* rsa, int bits, void* e, void* cb);
int RSA_sign(int type, const unsigned char* m, unsigned int m_len, unsigned char* sigret, unsigned int* siglen, void* rsa);
int RSA_verify(int type, const unsigned char* m, unsigned int m_len, const unsigned char* sig, unsigned int siglen, void* rsa);
int RSA_public_encrypt(int flen, const unsigned char* from, unsigned char* to, void* rsa, int padding);
int RSA_private_decrypt(int flen, const unsigned char* from, unsigned char* to, void* rsa, int padding);

// EC
void* EC_KEY_new_by_curve_name(int nid);
void EC_KEY_free(void* key);
int EC_KEY_set_public_key(void* key, const void* pub);
const void* EC_KEY_get0_public_key(const void* key);
void* EC_GROUP_new_by_curve_name(int nid);
void* EC_POINT_new(const void* group);
void EC_POINT_free(void* point);
int EC_POINT_mul(const void* group, void* r, const void* n, const void* m, void* ctx);

// NID constants
#define NID_X9_62_prime256v1 415

#endif
