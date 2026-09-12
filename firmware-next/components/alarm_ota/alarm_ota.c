#include "alarm_ota.h"
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include <string.h>

#define PREFIX_SIZE (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define TOKEN_CAP 256

static struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    alarm_ota_config_t config;
    char board[ALARM_OTA_NAME_CAP];
    uint8_t token[TOKEN_CAP];
    alarm_ota_state_t state;
    alarm_ota_handle_t generation;
    alarm_ota_manifest_t manifest;
    const esp_partition_t *target;
    esp_ota_handle_t writer;
    bool writer_open;
    uint32_t received;
    size_t prefix_size;
    uint8_t prefix[PREFIX_SIZE];
    mbedtls_sha256_context sha;
    int64_t deadline_us;
} g;

static bool supported_build(void) {
#if defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK) && CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
    return false; /* eFuse anti-rollback provisioning is outside this component. */
#endif
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE && \
    defined(CONFIG_ESP_TASK_WDT_INIT) && CONFIG_ESP_TASK_WDT_INIT && \
    defined(CONFIG_ESP_TASK_WDT_PANIC) && CONFIG_ESP_TASK_WDT_PANIC && \
    defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
    return true;
#else
    return false;
#endif
}

static esp_err_t lock(void) {
    if (!g.initialized) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(g.mutex, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
static void unlock(void) { xSemaphoreGive(g.mutex); }
static bool authorized(const void *request) { return g.config.authorize(request, g.config.user_context); }

static void discard(void) {
    if (g.writer_open) esp_ota_abort(g.writer);
    g.writer_open = false;
    mbedtls_sha256_free(&g.sha);
    g.state = ALARM_OTA_IDLE;
    g.received = 0;
    g.prefix_size = 0;
    g.target = NULL;
}

static esp_err_t safe_now(void) {
    alarm_ota_guard_t guard = {0};
    if (g.config.read_guard(&guard, g.config.user_context) != ESP_OK ||
        alarm_ota_check_guard(&guard, g.config.quiet_window_seconds) != ALARM_OTA_POLICY_OK)
        return ALARM_OTA_ERR_UNSAFE;
    return ESP_OK;
}

static esp_err_t active(alarm_ota_handle_t handle, const void *request) {
    if (!authorized(request)) return ALARM_OTA_ERR_AUTH;
    if (g.state == ALARM_OTA_IDLE || handle != g.generation) return ESP_ERR_INVALID_STATE;
    if (esp_timer_get_time() >= g.deadline_us) { discard(); return ESP_ERR_TIMEOUT; }
    esp_err_t err = safe_now();
    if (err != ESP_OK) discard();
    return err;
}

static bool exact_name(const char *image_text, size_t image_capacity, const char *expected) {
    size_t n = strnlen(image_text, image_capacity);
    return n < image_capacity && strlen(expected) == n && memcmp(image_text, expected, n) == 0;
}

static esp_err_t image_identity(const esp_app_desc_t *app) {
    if (app->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !exact_name(app->project_name, sizeof(app->project_name), g.manifest.board) ||
        !exact_name(app->version, sizeof(app->version), g.manifest.version))
        return ALARM_OTA_ERR_IMAGE_ID;
    return ESP_OK;
}

static esp_err_t check_prefix(void) {
    esp_image_header_t image;
    esp_app_desc_t app;
    memcpy(&image, g.prefix, sizeof(image));
    memcpy(&app, g.prefix + sizeof(image) + sizeof(esp_image_segment_header_t), sizeof(app));
    if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != ESP_CHIP_ID_ESP32S3)
        return ALARM_OTA_ERR_IMAGE_ID;
    return image_identity(&app);
}

static bool authentic_manifest(const alarm_ota_manifest_t *manifest) {
    char canonical[ALARM_OTA_CANONICAL_CAP]; uint8_t expected[32], actual[32];
    size_t n = alarm_ota_manifest_canonical(manifest, canonical, sizeof(canonical));
    if (!n || !alarm_ota_decode_digest(manifest->hmac_sha256, expected)) return false;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, g.token, g.config.device_token_length,
                             (const unsigned char *)canonical, n, actual) != 0) return false;
    return alarm_ota_digest_equal(expected, actual);
}

esp_err_t alarm_ota_init(const alarm_ota_config_t *config) {
    if (!supported_build()) return ESP_ERR_NOT_SUPPORTED;
    if (g.initialized) return ESP_ERR_INVALID_STATE;
    if (!config || !config->board_id || !config->device_token ||
        config->device_token_length < 16 || config->device_token_length > TOKEN_CAP ||
        !config->authorize || !config->read_guard || config->quiet_window_seconds < 300 ||
        config->quiet_window_seconds > 86400 || config->transfer_timeout_seconds < 30 ||
        config->transfer_timeout_seconds > 600 ||
        strnlen(config->board_id, ALARM_OTA_NAME_CAP) >= ALARM_OTA_NAME_CAP)
        return ESP_ERR_INVALID_ARG;
    const esp_app_desc_t *app = esp_app_get_description();
    uint32_t version[3];
    if (!app || !exact_name(app->project_name, sizeof(app->project_name), config->board_id) ||
        !alarm_ota_parse_version(app->version, sizeof(app->version), version))
        return ALARM_OTA_ERR_IMAGE_ID;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!running || !next || next->address == running->address ||
        running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX)
        return ESP_ERR_NOT_SUPPORTED;
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
    if (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) return ESP_ERR_INVALID_STATE;
    g.mutex = xSemaphoreCreateMutex();
    if (!g.mutex) return ESP_ERR_NO_MEM;
    g.config = *config;
    memcpy(g.board, config->board_id, strlen(config->board_id) + 1);
    memcpy(g.token, config->device_token, config->device_token_length);
    g.config.board_id = g.board;
    g.config.device_token = g.token;
    g.initialized = true;
    return ESP_OK;
}

esp_err_t alarm_ota_begin(const alarm_ota_manifest_t *manifest, const void *request, alarm_ota_handle_t *out) {
    if (!manifest || !out) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    if (!authorized(request)) { unlock(); return ALARM_OTA_ERR_AUTH; }
    if (g.state != ALARM_OTA_IDLE) { unlock(); return ESP_ERR_INVALID_STATE; }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *app = esp_app_get_description();
    if (!target || !running || target->address == running->address ||
        target->type != ESP_PARTITION_TYPE_APP ||
        target->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        target->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        unlock(); return ESP_ERR_NOT_SUPPORTED;
    }
    if (alarm_ota_check_manifest(manifest, g.board, app->version, target->size, PREFIX_SIZE) != ALARM_OTA_POLICY_OK) {
        unlock(); return ALARM_OTA_ERR_MANIFEST;
    }
    if (!authentic_manifest(manifest)) { unlock(); return ALARM_OTA_ERR_AUTH; }
    err = safe_now(); if (err != ESP_OK) { unlock(); return err; }
    g.manifest = *manifest;
    g.target = target;
    g.received = 0;
    g.prefix_size = 0;
    g.writer_open = false;
    g.deadline_us = esp_timer_get_time() + (int64_t)g.config.transfer_timeout_seconds * 1000000;
    mbedtls_sha256_init(&g.sha);
    if (mbedtls_sha256_starts(&g.sha, 0) != 0) { discard(); unlock(); return ESP_FAIL; }
    if (++g.generation == 0) ++g.generation;
    *out = g.generation;
    g.state = ALARM_OTA_RECEIVING;
    unlock(); return ESP_OK;
}

esp_err_t alarm_ota_write(alarm_ota_handle_t handle, const void *request, const void *bytes, size_t length) {
    if (!bytes || length > ALARM_OTA_MAX_CHUNK) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    err = active(handle, request);
    if (err != ESP_OK) { unlock(); return err; }
    if (g.state != ALARM_OTA_RECEIVING) { unlock(); return ESP_ERR_INVALID_STATE; }
    if (!alarm_ota_chunk_fits(g.received, length, g.manifest.size)) {
        discard(); unlock(); return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *data = bytes;
    size_t offset = 0;
    if (g.prefix_size < PREFIX_SIZE) {
        size_t take = PREFIX_SIZE - g.prefix_size;
        if (take > length) take = length;
        memcpy(g.prefix + g.prefix_size, data, take);
        g.prefix_size += take;
        offset = take;
        if (g.prefix_size == PREFIX_SIZE) {
            err = check_prefix();
            if (err == ESP_OK) err = esp_ota_begin(g.target, g.manifest.size, &g.writer);
            if (err == ESP_OK) {
                g.writer_open = true;
                err = esp_ota_write(g.writer, g.prefix, PREFIX_SIZE);
            }
        }
    }
    if (err == ESP_OK && offset < length) err = esp_ota_write(g.writer, data + offset, length - offset);
    if (err == ESP_OK && mbedtls_sha256_update(&g.sha, data, length) != 0) err = ESP_FAIL;
    if (err != ESP_OK) discard(); else g.received += (uint32_t)length;
    unlock(); return err;
}

esp_err_t alarm_ota_finish(alarm_ota_handle_t handle, const void *request) {
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    err = active(handle, request);
    if (err != ESP_OK) { unlock(); return err; }
    if (g.state != ALARM_OTA_RECEIVING || !g.writer_open || g.received != g.manifest.size) {
        discard(); unlock(); return ESP_ERR_INVALID_SIZE;
    }
    uint8_t actual[32], expected[32];
    if (mbedtls_sha256_finish(&g.sha, actual) != 0 ||
        !alarm_ota_decode_digest(g.manifest.sha256, expected) ||
        !alarm_ota_digest_equal(actual, expected)) {
        discard(); unlock(); return ALARM_OTA_ERR_DIGEST;
    }
    err = esp_ota_end(g.writer); /* Always consumes the IDF handle, even on failure. */
    g.writer_open = false;
    if (err == ESP_OK) {
        esp_app_desc_t desc;
        err = esp_ota_get_partition_description(g.target, &desc);
        if (err == ESP_OK) err = image_identity(&desc);
    }
    if (err != ESP_OK) discard();
    else { mbedtls_sha256_free(&g.sha); g.state = ALARM_OTA_VERIFIED; }
    unlock(); return err;
}

esp_err_t alarm_ota_activate(alarm_ota_handle_t handle, const void *request) {
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    err = active(handle, request);
    if (err != ESP_OK) { unlock(); return err; }
    if (g.state != ALARM_OTA_VERIFIED) { unlock(); return ESP_ERR_INVALID_STATE; }
    /* Caller serializes this operation against schedule mutations. No deferred
     * restart API: a verified image cannot remain armed past the checked window. */
    err = esp_ota_set_boot_partition(g.target);
    if (err != ESP_OK) { discard(); unlock(); return err; }
    esp_restart();
    return ESP_FAIL; /* esp_restart is noreturn in ESP-IDF. */
}

esp_err_t alarm_ota_abort(alarm_ota_handle_t handle, const void *request) {
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    if (!authorized(request)) { unlock(); return ALARM_OTA_ERR_AUTH; }
    if (g.state == ALARM_OTA_IDLE || handle != g.generation) { unlock(); return ESP_ERR_INVALID_STATE; }
    discard(); unlock(); return ESP_OK;
}

esp_err_t alarm_ota_get_status(alarm_ota_status_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    out->state = g.state;
    out->received = g.received;
    out->expected = g.state == ALARM_OTA_IDLE ? 0 : g.manifest.size;
    unlock(); return ESP_OK;
}

esp_err_t alarm_ota_maintenance(void) {
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    if (g.state != ALARM_OTA_IDLE) {
        err = esp_timer_get_time() >= g.deadline_us ? ESP_ERR_TIMEOUT : safe_now();
        if (err != ESP_OK) discard();
    }
    unlock(); return err;
}

static void pending_deadline(void *unused) { (void)unused; esp_restart(); }

esp_err_t alarm_ota_boot_self_test(alarm_ota_self_test_fn test, void *context, uint32_t timeout_ms) {
    if (!supported_build()) return ESP_ERR_NOT_SUPPORTED;
    if (!test || timeout_ms < 100 || timeout_ms > 30000) return ESP_ERR_INVALID_ARG;
#if defined(CONFIG_ESP_TASK_WDT_TIMEOUT_S)
    if ((uint64_t)timeout_ms > (uint64_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000) return ESP_ERR_INVALID_ARG;
#endif
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return ESP_ERR_NOT_FOUND;
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_ERR_NOT_FOUND || (err == ESP_OK && state != ESP_OTA_IMG_PENDING_VERIFY)) return ESP_OK;
    if (err != ESP_OK) return err;
    esp_task_wdt_user_handle_t watchdog;
    err = esp_task_wdt_add_user("alarm_ota_pending", &watchdog);
    if (err != ESP_OK) {
        esp_err_t rollback = esp_ota_mark_app_invalid_rollback_and_reboot();
        return rollback == ESP_OK ? err : rollback;
    }
    esp_timer_handle_t timer = NULL;
    const esp_timer_create_args_t args = { .callback = pending_deadline, .name = "ota_boot_deadline" };
    err = esp_timer_create(&args, &timer);
    int64_t start = esp_timer_get_time();
    if (err == ESP_OK) err = esp_timer_start_once(timer, (uint64_t)timeout_ms * 1000);
    if (err == ESP_OK) err = test(context);
    if (err == ESP_OK && esp_timer_get_time() - start >= (int64_t)timeout_ms * 1000) err = ESP_ERR_TIMEOUT;
    /* Keep watchdog + timer armed through the flash validity commit itself. */
    if (err == ESP_OK) err = esp_ota_mark_app_valid_cancel_rollback();
    if (timer) { esp_timer_stop(timer); esp_timer_delete(timer); }
    esp_task_wdt_delete_user(watchdog);
    if (err != ESP_OK) {
        esp_err_t rollback = esp_ota_mark_app_invalid_rollback_and_reboot();
        return rollback == ESP_OK ? err : rollback;
    }
    return ESP_OK;
}
