#pragma once
#include "esp_err.h"
#include <stddef.h>
typedef int nvs_handle_t;
#define NVS_READWRITE 1
static inline esp_err_t nvs_open(const char *s,int mode,nvs_handle_t *n){(void)s;(void)mode;*n=1;return ESP_OK;}
static inline esp_err_t nvs_set_blob(nvs_handle_t n,const char *k,const void *v,size_t len){(void)n;(void)k;(void)v;(void)len;return ESP_OK;}
static inline esp_err_t nvs_commit(nvs_handle_t n){(void)n;return ESP_OK;}
static inline void nvs_close(nvs_handle_t n){(void)n;}
