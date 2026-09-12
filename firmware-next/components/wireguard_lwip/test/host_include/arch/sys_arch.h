#pragma once
#include <stdbool.h>
typedef int sys_sem_t;
typedef int sys_mutex_t;
typedef int sys_mbox_t;
typedef int sys_thread_t;
#define LWIP_CORE_LOCK_QUERY_HOLDER 1
static inline bool sys_thread_tcpip(int query){(void)query;return true;}

typedef int sys_prot_t;
