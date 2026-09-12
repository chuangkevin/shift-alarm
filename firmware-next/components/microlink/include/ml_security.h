#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "lwip/pbuf.h"
#include <stdbool.h>
#include <stdint.h>
struct microlink_s;
typedef struct {
    uint32_t src, dst;
    uint16_t sport, dport;
    uint8_t proto;
    uint64_t until_ms;
} ml_flow_t;
typedef struct { uint32_t ip; int64_t expiry; uint8_t public_key[32]; uint16_t derp_region, derp_recv_region; uint64_t derp_recv_ms; } ml_allowed_peer_t;
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#define ML_POLICY_MAX_PEERS CONFIG_ML_MAX_PEERS
#else
#define ML_POLICY_MAX_PEERS 64
#endif
typedef struct {
    SemaphoreHandle_t lock;
    cJSON *filters;
    bool ready, authorized, reauth_requested, auth_pending;
    char auth_url[512];
    int64_t expiry;
    bool expired;
    bool peers_ready, peer_install_failed, capacity_exceeded;
    unsigned observed_peer_count;
    uint32_t peer_generation;
    ml_allowed_peer_t peers[ML_POLICY_MAX_PEERS];
    unsigned peer_count;
    ml_flow_t flows[16];
    uint16_t derp_home_region, derp_remote_connected;
    bool derp_home_connected;
    uint32_t derp_frames_tx, derp_frames_rx, derp_connect_failures, derp_capacity_drops, derp_queue_drops, derp_route_drops;
} ml_security_t;
bool ml_security_init(struct microlink_s *ml);
void ml_security_destroy(struct microlink_s *ml);
void ml_security_map(struct microlink_s *ml, cJSON *map);
void ml_security_close(struct microlink_s *ml);
int ml_security_register(struct microlink_s *ml, cJSON *response);
void ml_security_followup(struct microlink_s *ml, cJSON *request);
bool ml_security_packet(struct pbuf *p, bool outbound, void *ctx);

int64_t ml_parse_expiry(const char *s);
bool ml_peer_map_update(struct microlink_s *ml,cJSON *map);
