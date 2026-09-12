#pragma once
/* ESP-IDF's global DHCP/IPv6 callbacks must not interpret the WG driver state
 * as esp_netif_t. IDF 5.3 enables the separate client-data slot with PPP_SUPPORT.
 * This requires a complete configuration rebuild, not a per-file macro override. */
#if !defined(LWIP_ESP_NETIF_DATA) || !LWIP_ESP_NETIF_DATA
#error "Native WireGuard requires ESP-IDF client-data separation: enable CONFIG_LWIP_PPP_SUPPORT and rebuild all components"
#endif
