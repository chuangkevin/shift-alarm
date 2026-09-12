#pragma once
#include "lwip/tcpip.h"
#include "arch/sys_arch.h"
#if !LWIP_TCPIP_CORE_LOCKING
#error "alarm_tailnet requires CONFIG_LWIP_TCPIP_CORE_LOCKING=y"
#endif
/* Nested library calls and TCPIP callbacks already own this lock. */
typedef struct { bool acquired; } wg_core_guard_t;
static inline wg_core_guard_t wg_core_enter(void) {
    wg_core_guard_t g={.acquired=!sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER)};
    if(g.acquired)LOCK_TCPIP_CORE();
    return g;
}
static inline void wg_core_leave(wg_core_guard_t *g) {if(g->acquired)UNLOCK_TCPIP_CORE();}
#define WG_CORE_GUARD wg_core_guard_t wg_guard __attribute__((cleanup(wg_core_leave)))=wg_core_enter()
