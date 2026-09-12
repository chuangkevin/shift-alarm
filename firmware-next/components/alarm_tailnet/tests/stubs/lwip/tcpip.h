#pragma once
#define LWIP_TCPIP_CORE_LOCKING 1
void test_core_lock(void);
void test_core_unlock(void);
#define LOCK_TCPIP_CORE() test_core_lock()
#define UNLOCK_TCPIP_CORE() test_core_unlock()
