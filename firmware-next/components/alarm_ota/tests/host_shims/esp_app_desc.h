#pragma once
#include <stdint.h>
#include <stddef.h>
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432u
typedef struct {
    uint32_t magic_word,secure_version,reserv1[2];
    char version[32],project_name[32],time[16],date[16],idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint16_t min_efuse_blk_rev_full,max_efuse_blk_rev_full;
    uint32_t reserv2[19];
} esp_app_desc_t;
_Static_assert(sizeof(esp_app_desc_t)==256,"ESP-IDF 5.3.2 app descriptor ABI");
_Static_assert(offsetof(esp_app_desc_t,magic_word)==0,"ESP-IDF 5.3.2 descriptor magic offset");
_Static_assert(offsetof(esp_app_desc_t,version)==16,"ESP-IDF 5.3.2 version offset");
_Static_assert(offsetof(esp_app_desc_t,project_name)==48,"ESP-IDF 5.3.2 project offset");
const esp_app_desc_t *esp_app_get_description(void);
