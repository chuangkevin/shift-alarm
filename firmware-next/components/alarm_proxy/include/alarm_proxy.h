#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ALARM_PROXY_COMPONENT_VERSION "0.1.0"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool enabled;
    bool listening;
    uint16_t port;
    uint32_t active_connections;
    uint32_t completed_requests;
    uint32_t rejected_requests;
    char lan_ip[16];
} alarm_proxy_status_t;
/* Main serializes init/start/stop. Init only while stopped and workers drained.
 * Port 80 gateway: local routes to loopback 8081, allowlisted cloud routes to
 * fixed backend. Accept local subnets and ACL-filtered Tailnet traffic. Keep
 * running during AP setup. No credentials are stored or logged. */
esp_err_t alarm_proxy_init(const char *backend_ipv4, uint16_t backend_port);
const char *alarm_proxy_backend_host(void);
esp_err_t alarm_proxy_start(void);
esp_err_t alarm_proxy_stop(void);
esp_err_t alarm_proxy_get_status(alarm_proxy_status_t *out);
#ifdef __cplusplus
}
#endif
