#pragma once
#include <stdint.h>
#include <string.h>
struct pbuf {uint16_t tot_len;uint8_t *payload;};
static inline uint16_t pbuf_copy_partial(struct pbuf *p,void *d,uint16_t n,uint16_t off){if(n>p->tot_len-off)n=p->tot_len-off;memcpy(d,p->payload+off,n);return n;}
