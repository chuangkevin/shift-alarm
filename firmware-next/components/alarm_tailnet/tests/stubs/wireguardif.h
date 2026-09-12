#pragma once
#include "lwip/pbuf.h"
#include <stdbool.h>
typedef bool (*wireguard_packet_filter_fn)(struct pbuf *,bool,void *);
static inline void wireguardif_set_packet_filter(wireguard_packet_filter_fn f,void *ctx){(void)f;(void)ctx;}
