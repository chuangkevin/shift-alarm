#pragma once
#include "alarm_ota_policy.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ALARM_OTA_ERR_BASE 0x7B00
#define ALARM_OTA_ERR_AUTH (ALARM_OTA_ERR_BASE + 1)
#define ALARM_OTA_ERR_UNSAFE (ALARM_OTA_ERR_BASE + 2)
#define ALARM_OTA_ERR_MANIFEST (ALARM_OTA_ERR_BASE + 3)
#define ALARM_OTA_ERR_DIGEST (ALARM_OTA_ERR_BASE + 4)
#define ALARM_OTA_ERR_IMAGE_ID (ALARM_OTA_ERR_BASE + 5)
#define ALARM_OTA_MAX_CHUNK 16384

typedef uint32_t alarm_ota_handle_t;
typedef bool (*alarm_ota_authorize_fn)(const void *request_context, void *user_context);
typedef esp_err_t (*alarm_ota_guard_fn)(alarm_ota_guard_t *out, void *user_context);
typedef esp_err_t (*alarm_ota_self_test_fn)(void *user_context);

typedef struct {
    const char *board_id; /* Must equal this build's esp_app_desc.project_name. */
    const uint8_t *device_token; /* Copied internally; never returned or logged. */
    size_t device_token_length; /* 16..256 bytes; UTF-8 bearer token bytes, not hex-decoded. */
    uint32_t quiet_window_seconds; /* 300..86400; rejects a near alarm at every step. */
    uint32_t transfer_timeout_seconds; /* 30..600; total begin-to-activate deadline. */
    alarm_ota_authorize_fn authorize; /* Required; called on EVERY mutation. */
    alarm_ota_guard_fn read_guard; /* Required; includes all local/backend/snoozed alarms. */
    void *user_context;
} alarm_ota_config_t;

typedef enum {
    ALARM_OTA_IDLE = 0,
    ALARM_OTA_RECEIVING,
    ALARM_OTA_VERIFIED
} alarm_ota_state_t;

typedef struct {
    alarm_ota_state_t state;
    uint32_t received;
    uint32_t expected;
} alarm_ota_status_t;

/* Call once during app startup, before exposing routes. Requires IDF 5.3,
 * rollback-enabled bootloader, and panic-enabled initialized task watchdog.
 * Returns NOT_SUPPORTED when build configuration cannot provide these guarantees. */
esp_err_t alarm_ota_init(const alarm_ota_config_t *config);
/* Run on EVERY boot before init/routes. On a pending image, runs the supplied
 * self-test with a restart deadline + an unfed task-watchdog user. Only passing
 * diagnostics within the deadline mark the image valid. Failure rolls back.
 * Non-pending boot returns OK without running diagnostics. Timeout 100..30000ms,
 * no longer than CONFIG_ESP_TASK_WDT_TIMEOUT_S. Never require Internet for test. */
esp_err_t alarm_ota_boot_self_test(alarm_ota_self_test_fn self_test,
                                   void *user_context, uint32_t timeout_ms);
/* No erase until enough image header data arrives and board/version match.
 * Only the inactive OTA slot is ever written. At most one upload is accepted. */
esp_err_t alarm_ota_begin(const alarm_ota_manifest_t *manifest,
                          const void *request_context, alarm_ota_handle_t *out);
esp_err_t alarm_ota_write(alarm_ota_handle_t handle, const void *request_context,
                          const void *bytes, size_t length);
/* Exact size, full-image SHA256, esp_ota_end validation, then re-read app identity.
 * Does NOT select the new slot and does NOT restart. */
esp_err_t alarm_ota_finish(alarm_ota_handle_t handle, const void *request_context);
/* Rechecks authorization + alarm window immediately before selecting boot slot.
 * Successful call selects the verified image then esp_restart(); never returns. */
esp_err_t alarm_ota_activate(alarm_ota_handle_t handle, const void *request_context);
esp_err_t alarm_ota_abort(alarm_ota_handle_t handle, const void *request_context);
esp_err_t alarm_ota_get_status(alarm_ota_status_t *out);
/* Call periodically (e.g. 1Hz) from a worker independent of upload HTTP handling.
 * Cancels a timed-out/unsafe transfer, without erasing or changing boot selection. */
esp_err_t alarm_ota_maintenance(void);
#ifdef __cplusplus
}
#endif
