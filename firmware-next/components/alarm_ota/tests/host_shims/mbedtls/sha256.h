#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef ALARM_OTA_USE_OPENSSL
#include <openssl/evp.h>
typedef struct { EVP_MD_CTX *ctx; } mbedtls_sha256_context;
#else
typedef struct { uint32_t state[8]; uint64_t length; } mbedtls_sha256_context;
#endif
void mbedtls_sha256_init(mbedtls_sha256_context *ctx);
void mbedtls_sha256_free(mbedtls_sha256_context *ctx);
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx,int is224);
int mbedtls_sha256_update(mbedtls_sha256_context *ctx,const unsigned char *data,size_t size);
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx,unsigned char out[32]);
