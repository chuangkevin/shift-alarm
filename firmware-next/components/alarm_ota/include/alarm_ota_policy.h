#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ALARM_OTA_COMPONENT_VERSION "0.1.0"
#define ALARM_OTA_NAME_CAP 32
#define ALARM_OTA_DIGEST_CAP 65
#define ALARM_OTA_CANONICAL_CAP 160

typedef struct {
    char board[ALARM_OTA_NAME_CAP];
    char version[ALARM_OTA_NAME_CAP];
    uint32_t size;
    char sha256[ALARM_OTA_DIGEST_CAP];
    char hmac_sha256[ALARM_OTA_DIGEST_CAP];
} alarm_ota_manifest_t;

typedef struct {
    bool clock_valid;
    bool ringing;
    bool snoozed;
    bool schedule_ready;
    bool operator_confirmed_power; /* Current authenticated session explicitly confirms stable power; NOT a voltage measurement. */
    int64_t now_epoch;
    int64_t next_alarm_epoch; /* 0 means no upcoming alarm; must include snooze. */
} alarm_ota_guard_t;

typedef enum {
    ALARM_OTA_POLICY_OK = 0,
    ALARM_OTA_POLICY_BAD_MANIFEST,
    ALARM_OTA_POLICY_BOARD_MISMATCH,
    ALARM_OTA_POLICY_VERSION_REJECTED,
    ALARM_OTA_POLICY_SIZE_REJECTED,
    ALARM_OTA_POLICY_CLOCK_UNTRUSTED,
    ALARM_OTA_POLICY_ALARM_ACTIVE,
    ALARM_OTA_POLICY_ALARM_NEAR,
    ALARM_OTA_POLICY_POWER_UNSAFE
} alarm_ota_policy_result_t;

/* Strict numeric major.minor.patch; no prefixes, suffixes, or leading zeroes. */
bool alarm_ota_parse_version(const char *text, size_t capacity, uint32_t out[3]);
bool alarm_ota_decode_digest(const char hex[ALARM_OTA_DIGEST_CAP], uint8_t out[32]);
bool alarm_ota_digest_equal(const uint8_t a[32], const uint8_t b[32]);
alarm_ota_policy_result_t alarm_ota_check_manifest(
    const alarm_ota_manifest_t *manifest, const char *board, const char *current_version,
    uint32_t partition_size, uint32_t minimum_image_size);
/* Returns 0 on invalid input or insufficient output capacity. */
size_t alarm_ota_manifest_canonical(const alarm_ota_manifest_t *manifest, char *out, size_t capacity);
alarm_ota_policy_result_t alarm_ota_check_guard(const alarm_ota_guard_t *guard, uint32_t quiet_window_seconds);
bool alarm_ota_chunk_fits(uint32_t received, size_t length, uint32_t expected);
#ifdef __cplusplus
}
#endif
