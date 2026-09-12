#pragma once
#include <stdint.h>
#include "esp_err.h"
struct esp_netif_t {};
struct esp_ip4_addr_t { uint32_t addr; };
struct esp_netif_ip_info_t {esp_ip4_addr_t ip,netmask,gw;};
inline esp_netif_t test_net;
inline bool test_net_up=false;
inline esp_netif_ip_info_t test_ip{};
inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char*) {return &test_net;}
inline bool esp_netif_is_netif_up(esp_netif_t*) {return test_net_up;}
inline esp_err_t esp_netif_get_ip_info(esp_netif_t*,esp_netif_ip_info_t* ip) {*ip=test_ip;return ESP_OK;}
