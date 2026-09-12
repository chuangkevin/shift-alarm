#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Initialize NVS/network first. Lifecycle calls enqueue work and return promptly.
 * The worker waits for SNTP before registration. No auth key is required. */
typedef enum { ALARM_TAILNET_OFF, ALARM_TAILNET_CONNECTING, ALARM_TAILNET_AUTH_REQUIRED,
    ALARM_TAILNET_CONNECTED, ALARM_TAILNET_EXPIRED, ALARM_TAILNET_BLOCKED } alarm_tailnet_state_t;
typedef struct {
    alarm_tailnet_state_t state;
    char auth_url[512]; /* Treat as sensitive: local admin UI only; never log. */
    char ip[16];
    int64_t expires_at;
    bool acl_ready;
    uint32_t coord_stage, coord_last_reason, coord_reconnects, coord_successes;
    uint64_t coord_stage_since_ms, coord_last_failure_ms;
    bool policy_ready, peers_ready, node_authorized;
    uint32_t peer_generation;

    esp_err_t last_error;
    uint16_t peer_count, peer_capacity;
    bool capacity_exceeded;
    uint32_t wg_last_in_src, wg_last_in_dst, wg_last_in_port, wg_last_in_drop;
    uint32_t acl_rule_count, acl_range_count, acl_unsupported_count;
    uint32_t wg_out_packets, wg_out_dropped, wg_in_packets, wg_in_dropped, wg_rx_packets;
    uint32_t wg_netif_ip, wg_last_out_src, wg_last_out_dst; /* Host-order IPv4. */
    uint32_t wg_netif_mask, wg_lookup_misses, wg_derp_enqueue, wg_derp_enqueue_fail, wg_udp_tx;
    uint16_t wg_sessions, wg_peer_count;
    uint8_t wg_netif_index;
    bool wg_netif_up, wg_netif_link_up;
    /* Transport diagnostics; control-plane CONNECTED alone does not prove TCP. */
    bool derp_home_connected;
    uint16_t derp_home_region, derp_remote_connected;
    uint32_t derp_frames_tx, derp_frames_rx, derp_connect_failures;
    uint32_t derp_capacity_drops, derp_queue_drops, derp_route_drops;
} alarm_tailnet_status_t;
esp_err_t alarm_tailnet_start(const char *device_name);
esp_err_t alarm_tailnet_get_status(alarm_tailnet_status_t *out);
/* Starts a new interactive followup. Preserves persistent device identity. */
esp_err_t alarm_tailnet_reauth(void);
/* ESP_OK means request queued; observe status for completion/errors.
 * The worker alone owns the client. No HTTP server is started here. */
esp_err_t alarm_tailnet_stop(void);
#ifdef __cplusplus
}
#endif
