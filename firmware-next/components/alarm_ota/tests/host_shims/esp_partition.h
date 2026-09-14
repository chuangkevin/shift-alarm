#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_MIN 16
#define ESP_PARTITION_SUBTYPE_APP_OTA_MAX 32
typedef struct { uint32_t address; uint32_t size; uint8_t type; uint8_t subtype; } esp_partition_t;
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *out, size_t size);
