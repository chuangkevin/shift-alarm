#define _POSIX_C_SOURCE 200809L
#include "alarm_tailnet.h"
#include "microlink_internal.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
static void sleep_ms(unsigned ms){struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,NULL);}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
typedef struct {pthread_mutex_t lock;unsigned capacity,size,head,count;unsigned char bytes[1024];} queue_t;
QueueHandle_t xQueueCreate(unsigned capacity,unsigned size){queue_t *q=calloc(1,sizeof(*q));assert(capacity*size<=sizeof(q->bytes));pthread_mutex_init(&q->lock,NULL);q->capacity=capacity;q->size=size;return q;}
void vQueueDelete(QueueHandle_t q){free(q);}
int xQueueSend(void *arg,const void *item,unsigned timeout){(void)timeout;queue_t*q=arg;pthread_mutex_lock(&q->lock);int ok=q->count<q->capacity;if(ok){unsigned idx=(q->head+q->count)%q->capacity;memcpy(q->bytes+idx*q->size,item,q->size);q->count++;}pthread_mutex_unlock(&q->lock);return ok;}
int xQueueReceive(void *arg,void *item,unsigned timeout){queue_t*q=arg;double end=now()+timeout/1000.0;do{pthread_mutex_lock(&q->lock);if(q->count){memcpy(item,q->bytes+q->head*q->size,q->size);q->head=(q->head+1)%q->capacity;q->count--;pthread_mutex_unlock(&q->lock);return 1;}pthread_mutex_unlock(&q->lock);if(!timeout)break;sleep_ms(1);}while(now()<end);return 0;}
typedef struct {void(*fn)(void*);void*arg;} task_t;
static void *task_entry(void *arg){task_t t=*(task_t*)arg;free(arg);t.fn(t.arg);return NULL;}
int xTaskCreate(void(*fn)(void*),const char *name,unsigned stack,void*arg,unsigned priority,void*handle){(void)name;(void)stack;(void)priority;(void)handle;task_t*t=malloc(sizeof(*t));*t=(task_t){fn,arg};pthread_t p;int err=pthread_create(&p,NULL,task_entry,t);if(!err)pthread_detach(p);return !err;}
static atomic_int allocations,stops;
microlink_t *microlink_init(const microlink_config_t *c){assert(c->max_peers==64);if(atomic_fetch_add(&allocations,1)==0)return NULL;microlink_t*m=calloc(1,sizeof(*m));m->security.lock=xSemaphoreCreateMutex();m->security.ready=m->security.peers_ready=m->security.authorized=true;m->vpn_ip=0x64400001;return m;}
esp_err_t microlink_start(microlink_t*m){(void)m;return ESP_OK;}
esp_err_t microlink_stop(microlink_t*m){(void)m;atomic_fetch_add(&stops,1);sleep_ms(200);return ESP_OK;}
void microlink_destroy(microlink_t*m){vSemaphoreDelete(m->security.lock);free(m);}
void ml_security_close(microlink_t*m){m->security.ready=false;}
bool microlink_is_connected(const microlink_t*m){return m!=NULL;}
void microlink_ip_to_str(uint32_t ip,char*out){(void)ip;strcpy(out,"100.64.0.1");}
static alarm_tailnet_status_t wait_state(alarm_tailnet_state_t target){double end=now()+3;alarm_tailnet_status_t s;do{assert(alarm_tailnet_get_status(&s)==ESP_OK);if(s.state==target)return s;sleep_ms(5);}while(now()<end);fprintf(stderr,"state %d wanted %d error %d\n",s.state,target,s.last_error);assert(0);return s;}
int main(void){double begin=now();assert(alarm_tailnet_start("test-device")==ESP_OK);assert(now()-begin<0.1);wait_state(ALARM_TAILNET_BLOCKED);/* First allocation failed: manual join must recover without rotating an absent client. */assert(alarm_tailnet_reauth()==ESP_OK);wait_state(ALARM_TAILNET_CONNECTED);
 begin=now();assert(alarm_tailnet_reauth()==ESP_OK);assert(now()-begin<0.1);unsigned reads=0;double end=now()+0.15;while(now()<end){alarm_tailnet_status_t s;double t=now();assert(alarm_tailnet_get_status(&s)==ESP_OK);assert(now()-t<0.1);reads++;}assert(reads>100);wait_state(ALARM_TAILNET_CONNECTED);assert(atomic_load(&stops)>0);
 begin=now();assert(alarm_tailnet_stop()==ESP_OK);assert(now()-begin<0.1);wait_state(ALARM_TAILNET_OFF);puts("PASS: nonblocking lifecycle/status during slow stop, retry after init failure, worker-owned reauth/destroy");}
