#pragma once
#include <stdint.h>

namespace connectivity {
constexpr uint32_t backend_success_max_age_ms = 30000;

inline bool should_poll_backend(bool wifi_connected, bool tailnet_connected,
                                bool backend_configured, bool token_configured) {
    return wifi_connected && tailnet_connected && backend_configured && token_configured;
}

inline bool backend_reachable(bool wifi_connected, bool tailnet_connected,
                              bool /*last_result_ok*/, bool has_success,
                              uint32_t now_ms, uint32_t last_success_ms) {
    // One transient poll failure must not flip the UI to disconnected while a
    // recent successful heartbeat still proves that the route works.
    return wifi_connected && tailnet_connected && has_success &&
           uint32_t(now_ms - last_success_ms) <= backend_success_max_age_ms;
}
}
