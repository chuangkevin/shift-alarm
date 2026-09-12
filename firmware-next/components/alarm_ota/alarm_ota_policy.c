#include "alarm_ota_policy.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool bounded(const char *s, size_t cap, size_t *length) {
    if (!s || !cap) return false;
    for (size_t i = 0; i < cap; ++i) {
        if (!s[i]) { if (length) *length = i; return i != 0; }
    }
    return false;
}

static bool board_valid(const char *board) {
    size_t n;
    if (!bounded(board, ALARM_OTA_NAME_CAP, &n)) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = board[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

bool alarm_ota_parse_version(const char *text, size_t capacity, uint32_t out[3]) {
    size_t n, pos = 0;
    if (!out || !bounded(text, capacity, &n)) return false;
    for (unsigned part = 0; part < 3; ++part) {
        size_t start = pos;
        uint32_t value = 0;
        while (pos < n && text[pos] >= '0' && text[pos] <= '9') {
            unsigned digit = (unsigned)(text[pos++] - '0');
            if (value > (UINT32_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        }
        if (pos == start || (pos - start > 1 && text[start] == '0')) return false;
        out[part] = value;
        if (part < 2) { if (pos >= n || text[pos++] != '.') return false; }
    }
    return pos == n;
}

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool alarm_ota_decode_digest(const char hex[ALARM_OTA_DIGEST_CAP], uint8_t out[32]) {
    size_t n;
    if (!out || !bounded(hex, ALARM_OTA_DIGEST_CAP, &n) || n != 64) return false;
    for (unsigned i = 0; i < 32; ++i) {
        int high = nibble(hex[2 * i]), low = nibble(hex[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

bool alarm_ota_digest_equal(const uint8_t a[32], const uint8_t b[32]) {
    if (!a || !b) return false;
    volatile uint8_t different = 0;
    for (unsigned i = 0; i < 32; ++i) different |= a[i] ^ b[i];
    return different == 0;
}

alarm_ota_policy_result_t alarm_ota_check_manifest(
    const alarm_ota_manifest_t *m, const char *board, const char *current_version,
    uint32_t partition_size, uint32_t minimum_image_size) {
    uint32_t incoming[3], current[3]; uint8_t digest[32];
    if (!m || !board_valid(m->board) || !board_valid(board) ||
        !alarm_ota_parse_version(m->version, sizeof(m->version), incoming) ||
        !alarm_ota_parse_version(current_version, ALARM_OTA_NAME_CAP, current) ||
        !alarm_ota_decode_digest(m->sha256, digest) ||
        !alarm_ota_decode_digest(m->hmac_sha256, digest)) return ALARM_OTA_POLICY_BAD_MANIFEST;
    if (strcmp(m->board, board)) return ALARM_OTA_POLICY_BOARD_MISMATCH;
    int comparison = 0;
    for (unsigned i = 0; i < 3 && !comparison; ++i)
        comparison = incoming[i] > current[i] ? 1 : incoming[i] < current[i] ? -1 : 0;
    if (comparison <= 0) return ALARM_OTA_POLICY_VERSION_REJECTED;
    if (!m->size || m->size < minimum_image_size || m->size > partition_size)
        return ALARM_OTA_POLICY_SIZE_REJECTED;
    return ALARM_OTA_POLICY_OK;
}

size_t alarm_ota_manifest_canonical(const alarm_ota_manifest_t *m, char *out, size_t capacity) {
    uint32_t version[3]; uint8_t digest[32];
    if (!m || !out || !board_valid(m->board) || !m->size ||
        !alarm_ota_parse_version(m->version, sizeof(m->version), version) ||
        !alarm_ota_decode_digest(m->sha256, digest)) return 0;
    int n = snprintf(out, capacity, "%s\n%s\n%lu\n%s\n", m->board, m->version,
                     (unsigned long)m->size, m->sha256);
    return n > 0 && (size_t)n < capacity ? (size_t)n : 0;
}

alarm_ota_policy_result_t alarm_ota_check_guard(const alarm_ota_guard_t *g, uint32_t window) {
    if (!g || !g->clock_valid || !g->schedule_ready || g->now_epoch < 1704067200)
        return ALARM_OTA_POLICY_CLOCK_UNTRUSTED;
    if (!g->operator_confirmed_power) return ALARM_OTA_POLICY_POWER_UNSAFE;
    if (g->ringing || g->snoozed) return ALARM_OTA_POLICY_ALARM_ACTIVE;
    if (g->next_alarm_epoch < 0) return ALARM_OTA_POLICY_CLOCK_UNTRUSTED;
    if (g->next_alarm_epoch && (g->next_alarm_epoch <= g->now_epoch ||
        (uint64_t)(g->next_alarm_epoch - g->now_epoch) <= window))
        return ALARM_OTA_POLICY_ALARM_NEAR;
    return ALARM_OTA_POLICY_OK;
}

bool alarm_ota_chunk_fits(uint32_t received, size_t length, uint32_t expected) {
    return length > 0 && received <= expected && length <= (size_t)(expected - received);
}
