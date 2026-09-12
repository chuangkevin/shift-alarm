#pragma once
#include <string.h>
static inline void esp_fill_random(void *p,size_t n){memset(p,0x5a,n);}
