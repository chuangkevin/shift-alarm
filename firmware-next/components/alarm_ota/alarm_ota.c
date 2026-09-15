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

#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (ms)
#endif

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
    alarm_ota_staged_record_t staged;
    bool staged_valid;
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
    return xSemaphoreTake(g.mutex, pdMS_TO_TICKS(250)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
static void unlock(void) { xSemaphoreGive(g.mutex); }
static bool authorized(const void *request) { return g.config.authorize(request, g.config.user_context); }

static void discard_transfer(void) {
    if (g.writer_open) esp_ota_abort(g.writer);
    g.writer_open = false;
    mbedtls_sha256_free(&g.sha);
    g.state = g.staged_valid ? ALARM_OTA_STAGED : ALARM_OTA_IDLE;
    g.received = 0;
    g.prefix_size = 0;
    g.target = NULL;
}

static void marker_fault(void) {
    memset(&g.staged, 0, sizeof(g.staged));
    g.staged_valid = false;
    g.state = ALARM_OTA_MARKER_FAULT;
    g.received = 0;
    g.prefix_size = 0;
    g.target = NULL;
}

static esp_err_t safe_now(void) {
    alarm_ota_guard_t guard = {0};
    if (g.config.read_guard(&guard, g.config.user_context) != ESP_OK) return ALARM_OTA_ERR_UNSAFE;
    alarm_ota_policy_result_t result = alarm_ota_check_guard(&guard, g.config.quiet_window_seconds);
    if (result == ALARM_OTA_POLICY_CHARGING_REQUIRED) return ALARM_OTA_ERR_CHARGING_REQUIRED;
    if (result != ALARM_OTA_POLICY_OK) return ALARM_OTA_ERR_UNSAFE;
    return ESP_OK;
}

static esp_err_t active(alarm_ota_handle_t handle, const void *request) {
    if (!authorized(request)) return ALARM_OTA_ERR_AUTH;
    if (g.state == ALARM_OTA_IDLE || handle != g.generation) return ESP_ERR_INVALID_STATE;
    if (esp_timer_get_time() >= g.deadline_us) { discard_transfer(); return ESP_ERR_TIMEOUT; }
    esp_err_t err = safe_now();
    if (err != ESP_OK) discard_transfer();
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

static void encode_digest(const uint8_t digest[32], char out[ALARM_OTA_DIGEST_CAP]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) { out[i * 2] = hex[digest[i] >> 4]; out[i * 2 + 1] = hex[digest[i] & 15]; }
    out[64] = 0;
}

static bool staged_authentic(const alarm_ota_staged_record_t *record) {
    char canonical[ALARM_OTA_STAGED_CANONICAL_CAP]; uint8_t expected[32], actual[32];
    size_t n = alarm_ota_staged_canonical(record, canonical, sizeof(canonical));
    if (!n || !alarm_ota_decode_digest(record->record_hmac_sha256, expected)) return false;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return md && mbedtls_md_hmac(md, g.token, g.config.device_token_length,
        (const unsigned char *)canonical, n, actual) == 0 && alarm_ota_digest_equal(expected, actual);
}

static bool target_for_record(const alarm_ota_staged_record_t *record, const esp_partition_t **out) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!running || !target || target->address == running->address ||
        target->type != ESP_PARTITION_TYPE_APP ||
        target->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN || target->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX ||
        target->subtype != record->target_subtype || target->address != record->target_address) return false;
    if (out) *out = target;
    return true;
}

static bool staged_record_valid(const alarm_ota_staged_record_t *record) {
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *target = NULL;
    esp_app_desc_t desc;
    if (!alarm_ota_staged_record_shape_valid(record) || !authentic_manifest(&record->manifest) ||
        !staged_authentic(record) || !app || !target_for_record(record, &target) ||
        alarm_ota_check_manifest(&record->manifest, g.board, app->version,
            target->size, PREFIX_SIZE) != ALARM_OTA_POLICY_OK ||
        esp_ota_get_partition_description(target, &desc) != ESP_OK) return false;
    alarm_ota_manifest_t previous = g.manifest;
    g.manifest = record->manifest;
    bool valid = image_identity(&desc) == ESP_OK;
    if (!valid) g.manifest = previous;
    return valid;
}

static void accept_staged(const alarm_ota_staged_record_t *record) {
    g.manifest = record->manifest;
    g.staged = *record;
    g.staged_valid = true;
    g.state = ALARM_OTA_STAGED;
    g.received = 0;
    g.target = NULL;
}

static esp_err_t observe_marker(const alarm_ota_staged_record_t *expected) {
    alarm_ota_staged_record_t record = {0};
    esp_err_t err = g.config.marker_load(&record, g.config.user_context);
    if (err == ESP_ERR_NOT_FOUND) {
        memset(&g.staged, 0, sizeof(g.staged));
        g.staged_valid = false;
        g.state = ALARM_OTA_IDLE;
        g.received = 0;
        g.target = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK || !staged_record_valid(&record) ||
        (expected && memcmp(expected, &record, sizeof(record)) != 0)) {
        marker_fault();
        return ALARM_OTA_ERR_MARKER;
    }
    accept_staged(&record);
    return ESP_OK;
}

static esp_err_t clear_and_observe(void) {
    esp_err_t clear_err = g.config.marker_clear(g.config.user_context);
    esp_err_t observed = observe_marker(NULL);
    if (observed == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (observed == ESP_OK) return clear_err == ESP_OK ? ALARM_OTA_ERR_MARKER : clear_err;
    marker_fault();
    return clear_err == ESP_OK ? ALARM_OTA_ERR_MARKER : clear_err;
}

esp_err_t alarm_ota_init(const alarm_ota_config_t *config) {
    if (!supported_build()) return ESP_ERR_NOT_SUPPORTED;
    if (g.initialized) return ESP_ERR_INVALID_STATE;
    if (!config || !config->board_id || !config->device_token ||
        config->device_token_length < 16 || config->device_token_length > TOKEN_CAP ||
        !config->authorize || !config->read_guard || !config->marker_load || !config->marker_store ||
        !config->marker_clear || config->quiet_window_seconds < 300 ||
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
    g.state = ALARM_OTA_MARKER_FAULT;
    g.initialized = true;
    return ESP_OK;
}

esp_err_t alarm_ota_begin(const alarm_ota_manifest_t *manifest, const void *request, alarm_ota_handle_t *out) {
    if (!manifest || !out) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    if (!authorized(request)) { unlock(); return ALARM_OTA_ERR_AUTH; }
    if (g.state == ALARM_OTA_MARKER_FAULT) { unlock(); return ALARM_OTA_ERR_MARKER; }
    if (g.state != ALARM_OTA_IDLE || g.staged_valid) { unlock(); return ESP_ERR_INVALID_STATE; }
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
    if (mbedtls_sha256_starts(&g.sha, 0) != 0) { discard_transfer(); unlock(); return ESP_FAIL; }
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
        discard_transfer(); unlock(); return ESP_ERR_INVALID_SIZE;
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
    if (err != ESP_OK) discard_transfer(); else g.received += (uint32_t)length;
    unlock(); return err;
}

esp_err_t alarm_ota_finish(alarm_ota_handle_t handle, const void *request) {
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    err = active(handle, request);
    if (err != ESP_OK) { unlock(); return err; }
    if (g.state != ALARM_OTA_RECEIVING || !g.writer_open || g.received != g.manifest.size) {
        discard_transfer(); unlock(); return ESP_ERR_INVALID_SIZE;
    }
    uint8_t actual[32], expected[32];
    if (mbedtls_sha256_finish(&g.sha, actual) != 0 ||
        !alarm_ota_decode_digest(g.manifest.sha256, expected) ||
        !alarm_ota_digest_equal(actual, expected)) {
        discard_transfer(); unlock(); return ALARM_OTA_ERR_DIGEST;
    }
    err = esp_ota_end(g.writer); /* Always consumes the IDF handle, even on failure. */
    g.writer_open = false;
    if (err == ESP_OK) {
        esp_app_desc_t desc;
        err = esp_ota_get_partition_description(g.target, &desc);
        if (err == ESP_OK) err = image_identity(&desc);
    }
    if (err == ESP_OK) err = safe_now();
    if (err == ESP_OK) {
        alarm_ota_staged_record_t record = {0}; uint8_t mac[32];
        record.schema = ALARM_OTA_STAGED_SCHEMA; record.manifest = g.manifest;
        record.target_subtype = g.target->subtype; record.target_address = g.target->address;
        char canonical[ALARM_OTA_STAGED_CANONICAL_CAP];
        size_t n = alarm_ota_staged_canonical(&record, canonical, sizeof(canonical));
        const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (!n || !md || mbedtls_md_hmac(md, g.token, g.config.device_token_length,
            (const unsigned char *)canonical, n, mac) != 0) err = ALARM_OTA_ERR_MARKER;
        else {
            encode_digest(mac, record.record_hmac_sha256);
            esp_err_t store_err = g.config.marker_store(&record, g.config.user_context);
            esp_err_t observed = observe_marker(&record);
            if (observed == ESP_OK) err = ESP_OK;
            else if (observed == ESP_ERR_NOT_FOUND) {
                if (store_err == ESP_OK) { marker_fault(); err = ALARM_OTA_ERR_MARKER; }
                else err = store_err;
            }
            else err = ALARM_OTA_ERR_MARKER;
        }
    }
    if (err != ESP_OK) {
        mbedtls_sha256_free(&g.sha); g.writer_open = false; g.received = 0;
        g.prefix_size = 0; g.target = NULL;
        if (g.state != ALARM_OTA_IDLE && g.state != ALARM_OTA_STAGED) marker_fault();
    }
    else { mbedtls_sha256_free(&g.sha); g.writer_open = false; g.target = NULL; g.received = 0; g.prefix_size = 0; g.state = ALARM_OTA_STAGED; }
    unlock(); return err;
}

esp_err_t alarm_ota_load_staged(void) {
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    if (g.state == ALARM_OTA_RECEIVING) { unlock(); return ESP_ERR_INVALID_STATE; }
    alarm_ota_staged_record_t record = {0}; err = g.config.marker_load(&record, g.config.user_context);
    if (err == ESP_ERR_NOT_FOUND) {
        memset(&g.staged, 0, sizeof(g.staged));g.staged_valid=false;g.state=ALARM_OTA_IDLE;g.received=0;g.target=NULL;
        unlock();return ESP_OK;
    }
    if (err != ESP_OK) { marker_fault(); unlock(); return ALARM_OTA_ERR_MARKER; }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && running->address == record.target_address) { marker_fault(); unlock(); return ALARM_OTA_ERR_MARKER; }
    if (staged_record_valid(&record)) { accept_staged(&record); unlock(); return ESP_OK; }
    marker_fault();unlock();return ALARM_OTA_ERR_MARKER;
}

typedef struct { const esp_partition_t *target; esp_err_t error; alarm_ota_staged_record_t record; } activation_context_t;
static bool activation_auth(void *p, const alarm_ota_staged_record_t *r) {(void)p;return authentic_manifest(&r->manifest)&&staged_authentic(r);}
static bool activation_guard(void *p) {activation_context_t *c=p;c->error=safe_now();return c->error==ESP_OK;}
static bool activation_target(void *p, const alarm_ota_staged_record_t *r) {activation_context_t *c=p;bool ok=target_for_record(r,&c->target);if(!ok)c->error=ALARM_OTA_ERR_MARKER;return ok;}
static bool activation_image(void *p, const alarm_ota_staged_record_t *r) {
    activation_context_t *c=p;c->error=ESP_OK;mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);uint8_t actual[32],expected[32],buffer[4096];
    bool ok=mbedtls_sha256_starts(&sha,0)==0;
    for(uint32_t offset=0;ok&&offset<r->manifest.size;offset+=sizeof(buffer)){
        size_t count=r->manifest.size-offset;if(count>sizeof(buffer))count=sizeof(buffer);
        c->error=safe_now();if(c->error!=ESP_OK||esp_partition_read(c->target,offset,buffer,count)!=ESP_OK||mbedtls_sha256_update(&sha,buffer,count)!=0)ok=false;
    }
    if(ok)ok=mbedtls_sha256_finish(&sha,actual)==0&&alarm_ota_decode_digest(r->manifest.sha256,expected)&&alarm_ota_digest_equal(actual,expected);
    mbedtls_sha256_free(&sha);esp_app_desc_t desc;
    if(ok)ok=esp_ota_get_partition_description(c->target,&desc)==ESP_OK&&image_identity(&desc)==ESP_OK;
    if (!ok && c->error == ESP_OK) c->error = ALARM_OTA_ERR_DIGEST;
    return ok;
}
static bool activation_clear(void *p) {activation_context_t *c=p;c->error=clear_and_observe();return c->error==ESP_OK;}
static bool activation_boot(void *p) {
    activation_context_t *c=p;c->error=safe_now();
    if(c->error!=ESP_OK){
        esp_err_t restored=g.config.marker_store(&c->record,g.config.user_context);
        esp_err_t observed=observe_marker(&c->record);
        if(observed==ESP_OK)c->error=ALARM_OTA_ERR_UNSAFE;
        else if(observed==ESP_ERR_NOT_FOUND)c->error=restored==ESP_OK?ALARM_OTA_ERR_MARKER:restored;
        else {marker_fault();c->error=restored==ESP_OK?ALARM_OTA_ERR_MARKER:restored;}
        return false;
    }
    c->error=esp_ota_set_boot_partition(c->target);return c->error==ESP_OK;
}

esp_err_t alarm_ota_activate(const void *request) {
    esp_err_t err=lock();if(err!=ESP_OK)return err;
    if(!authorized(request)){unlock();return ALARM_OTA_ERR_AUTH;}
    if(g.state!=ALARM_OTA_STAGED||!g.staged_valid){unlock();return ESP_ERR_INVALID_STATE;}
    activation_context_t context={NULL,ALARM_OTA_ERR_MARKER,g.staged};
    const alarm_ota_activation_ops_t ops={activation_auth,activation_target,activation_image,activation_guard,activation_clear,activation_boot};
    alarm_ota_activation_result_t result=alarm_ota_run_activation(&context.record,&ops,&context);
    if(result!=ALARM_OTA_ACTIVATION_OK){
        err=context.error;
        bool guard_blocked=err==ALARM_OTA_ERR_CHARGING_REQUIRED||err==ALARM_OTA_ERR_UNSAFE;
        if(!guard_blocked&&(result==ALARM_OTA_ACTIVATION_BAD_RECORD||result==ALARM_OTA_ACTIVATION_TARGET_INVALID||result==ALARM_OTA_ACTIVATION_IMAGE_INVALID)){marker_fault();err=ALARM_OTA_ERR_MARKER;}
        unlock();return err;
    }
    unlock();esp_restart();return ESP_FAIL;
}

esp_err_t alarm_ota_discard_staged(const void *request) {
    esp_err_t err=lock();if(err!=ESP_OK)return err;
    if(!authorized(request)){unlock();return ALARM_OTA_ERR_AUTH;}
    if(g.state!=ALARM_OTA_STAGED&&g.state!=ALARM_OTA_MARKER_FAULT){unlock();return ESP_ERR_NOT_FOUND;}
    err=clear_and_observe();unlock();return err;
}

esp_err_t alarm_ota_abort(alarm_ota_handle_t handle, const void *request) {
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    if (!authorized(request)) { unlock(); return ALARM_OTA_ERR_AUTH; }
    if (g.state != ALARM_OTA_RECEIVING || handle != g.generation) { unlock(); return ESP_ERR_INVALID_STATE; }
    discard_transfer(); unlock(); return ESP_OK;
}

esp_err_t alarm_ota_get_status(alarm_ota_status_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    out->state = g.state;
    out->received = g.received;
    out->expected = (g.state == ALARM_OTA_RECEIVING || g.state == ALARM_OTA_STAGED) ? g.manifest.size : 0;
    out->staged_valid = g.staged_valid;
    out->marker_fault = g.state == ALARM_OTA_MARKER_FAULT;
    out->staged = g.staged;
    unlock(); return ESP_OK;
}

esp_err_t alarm_ota_maintenance(void) {
    esp_err_t err = lock(); if (err != ESP_OK) return err;
    if (g.state == ALARM_OTA_RECEIVING) {
        if (esp_timer_get_time() >= g.deadline_us) {
            err = ESP_ERR_TIMEOUT;
            discard_transfer();
        } else {
            /* A sampled guard can change while the HTTP worker owns the transfer.
             * Report it, but let the worker's per-write guard make the decision.
             * This prevents the main loop from silently invalidating the handle
             * between two otherwise valid chunks. */
            err = safe_now();
        }
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
