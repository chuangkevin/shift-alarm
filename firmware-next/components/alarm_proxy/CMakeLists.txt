idf_component_register(SRCS "alarm_proxy.cpp" "proxy_http.cpp" INCLUDE_DIRS "include"
    REQUIRES alarm_tailnet esp_netif esp_timer lwip)
