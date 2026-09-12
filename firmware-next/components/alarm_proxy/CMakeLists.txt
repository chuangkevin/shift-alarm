idf_component_register(SRCS "alarm_proxy.cpp" "proxy_http.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_timer lwip)
