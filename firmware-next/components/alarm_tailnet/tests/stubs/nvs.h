#pragma once
#include "esp_err.h"
#include <stddef.h>
typedef int nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *s,int mode,nvs_handle_t *n);
esp_err_t nvs_set_blob(nvs_handle_t n,const char *k,const void *v,size_t len);
esp_err_t nvs_commit(nvs_handle_t n);
void nvs_close(nvs_handle_t n);
