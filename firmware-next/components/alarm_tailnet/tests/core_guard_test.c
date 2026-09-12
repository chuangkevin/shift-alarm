#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include "wg_core_guard.h"
static pthread_mutex_t core=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool held;
static unsigned counter;
void test_core_lock(void){assert(!held);pthread_mutex_lock(&core);held=true;}
void test_core_unlock(void){assert(held);held=false;pthread_mutex_unlock(&core);}
int sys_thread_tcpip(int query){(void)query;return held;}
static void nested(void){WG_CORE_GUARD;assert(held);counter++;}
static void early_return(void){WG_CORE_GUARD;return;}
static void *worker(void *arg){(void)arg;for(unsigned i=0;i<10000;i++){WG_CORE_GUARD;nested();}assert(!held);return NULL;}
int main(void){pthread_t threads[4];early_return();assert(!held);for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,worker,NULL));for(unsigned i=0;i<4;i++)pthread_join(threads[i],NULL);assert(counter==40000);assert(!pthread_mutex_trylock(&core));pthread_mutex_unlock(&core);puts("PASS: core guard serializes 4 threads, supports nested calls and early returns");}
