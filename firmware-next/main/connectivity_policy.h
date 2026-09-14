#pragma once
#include <stdint.h>

namespace connectivity {
constexpr uint32_t backend_success_max_age_ms = 30000;

inline bool backend_reachable(bool wifi_connected, bool last_result_ok, bool has_success,
                              uint32_t now_ms, uint32_t last_success_ms) {
    return wifi_connected && last_result_ok && has_success &&
           uint32_t(now_ms - last_success_ms) <= backend_success_max_age_ms;
}
}
