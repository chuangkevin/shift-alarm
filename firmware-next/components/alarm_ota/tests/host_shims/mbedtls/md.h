#pragma once
#include <stddef.h>
#define MBEDTLS_MD_SHA256 1
typedef struct { int type; } mbedtls_md_info_t;
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type);
int mbedtls_md_hmac(const mbedtls_md_info_t *md,const unsigned char *key,size_t key_len,
                    const unsigned char *input,size_t input_len,unsigned char out[32]);
