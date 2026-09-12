#pragma once
#include <assert.h>
#include <stdio.h>
#define LWIP_PLATFORM_DIAG(x) do {} while(0)
#define LWIP_PLATFORM_ASSERT(x) assert(!x)
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#undef htons
#undef ntohs
#undef htonl
#undef ntohl
