idf_component_register(SRCS "alarm_ota.c" "alarm_ota_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_update bootloader_support esp_app_format esp_partition esp_system esp_timer freertos mbedtls)
