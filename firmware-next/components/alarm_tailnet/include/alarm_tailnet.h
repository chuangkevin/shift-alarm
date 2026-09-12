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
    esp_err_t last_error;
    uint16_t peer_count, peer_capacity;
    bool capacity_exceeded;
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
