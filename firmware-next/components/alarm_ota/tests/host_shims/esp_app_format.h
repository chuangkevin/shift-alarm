#pragma once
#include <stdint.h>
#include <stddef.h>
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_CHIP_ID_ESP32S3 9
typedef uint16_t esp_chip_id_t;
typedef struct {
    uint8_t magic,segment_count,spi_mode,spi_speed_size;
    uint32_t entry_addr;
    uint8_t wp_pin,spi_pin_drv[3];
    esp_chip_id_t chip_id;
    uint8_t min_chip_rev;
    uint16_t min_chip_rev_full,max_chip_rev_full;
    uint8_t reserved[4],hash_appended;
} __attribute__((packed)) esp_image_header_t;
typedef struct { uint32_t load_addr,data_len; } esp_image_segment_header_t;
_Static_assert(sizeof(esp_image_header_t)==24,"ESP-IDF 5.3.2 image header ABI");
_Static_assert(offsetof(esp_image_header_t,magic)==0,"ESP-IDF 5.3.2 image magic offset");
_Static_assert(offsetof(esp_image_header_t,chip_id)==12,"ESP-IDF 5.3.2 chip ID offset");
_Static_assert(sizeof(esp_image_segment_header_t)==8,"ESP-IDF 5.3.2 segment header ABI");
