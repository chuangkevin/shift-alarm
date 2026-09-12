#pragma once
#include "ml_security.h"
#include <stdlib.h>
#define ML_MAX_ENDPOINTS 8
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
typedef struct {
 enum {ML_PEER_ADD,ML_PEER_REMOVE,ML_PEER_UPDATE_ENDPOINT,ML_PEER_RESET,ML_PEER_SYNC_DONE} action;
 uint64_t node_id;uint32_t generation,vpn_ip;uint8_t public_key[32],disco_key[32];
 char hostname[64];uint16_t derp_region;
 struct {uint32_t ip;uint16_t port;bool is_ipv6;} endpoints[ML_MAX_ENDPOINTS];
 int endpoint_count;
} ml_peer_update_t;
typedef struct microlink_s {
 ml_security_t security;uint32_t vpn_ip;bool key_expired;int64_t key_expiry_epoch;
 uint8_t wg_public_key[32];
 cJSON *peer_map;bool peer_map_dirty;void *peer_update_queue;
} microlink_t;
static inline void *ml_psram_malloc(size_t n){return malloc(n);}
int xQueueSend(void *queue,const void *item,unsigned timeout);
uint64_t ml_get_time_ms(void);
#include "esp_err.h"
typedef void *QueueHandle_t;
typedef struct {const char *device_name,*auth_key;bool enable_derp;unsigned max_peers;} microlink_config_t;
#define ML_MAX_PEERS 64
QueueHandle_t xQueueCreate(unsigned capacity,unsigned size);
void vQueueDelete(QueueHandle_t q);
int xQueueReceive(QueueHandle_t q,void *item,unsigned timeout);
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle);
microlink_t *microlink_init(const microlink_config_t *config);
esp_err_t microlink_start(microlink_t *m);
esp_err_t microlink_stop(microlink_t *m);
void microlink_destroy(microlink_t *m);
bool microlink_is_connected(const microlink_t *m);
void microlink_ip_to_str(uint32_t ip,char *out);
