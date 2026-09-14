#pragma once
#include <stdint.h>

namespace battery {
constexpr uint32_t startup_interval_ms = 1000;
constexpr uint32_t steady_interval_ms = 60000;
constexpr uint32_t stale_after_ms = 300000;

struct State {
    uint16_t samples[3] = {};
    uint8_t sample_count = 0;
    uint8_t next_index = 0;
    uint32_t last_attempt_ms = 0;
    uint32_t last_valid_ms = 0;
    bool attempted = false;
};

inline bool charging_active(int gpio_level) {
    return gpio_level == 1;
}

inline int percent(uint16_t raw) {
    constexpr uint16_t points[] = {1970, 2062, 2154, 2246, 2338, 2430};
    if (raw <= points[0]) return 0;
    if (raw >= points[5]) return 100;
    for (int i = 1; i < 6; ++i) {
        if (raw <= points[i]) return (i - 1) * 20 +
            (int(raw - points[i - 1]) * 20 + int(points[i] - points[i - 1]) / 2) /
                int(points[i] - points[i - 1]);
    }
    return 100;
}

inline bool should_sample(const State &state, uint32_t now) {
    const uint32_t interval = state.sample_count < 3 ? startup_interval_ms : steady_interval_ms;
    return !state.attempted || uint32_t(now - state.last_attempt_ms) >= interval;
}

inline void record(State &state, uint32_t now, bool valid, uint16_t raw = 0) {
    state.attempted = true;
    state.last_attempt_ms = now;
    if (!valid) return;
    state.samples[state.next_index] = raw;
    state.next_index = (state.next_index + 1) % 3;
    if (state.sample_count < 3) ++state.sample_count;
    state.last_valid_ms = now;
}

inline bool value(const State &state, uint32_t now, int &out_percent, uint32_t &age_seconds) {
    if (!state.sample_count || uint32_t(now - state.last_valid_ms) > stale_after_ms) return false;
    uint16_t sorted[3] = {};
    for (uint8_t i = 0; i < state.sample_count; ++i) sorted[i] = state.samples[i];
    for (uint8_t i = 1; i < state.sample_count; ++i)
        for (uint8_t j = i; j > 0 && sorted[j] < sorted[j - 1]; --j) {
            uint16_t temporary = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = temporary;
        }
    const uint16_t filtered = state.sample_count == 1 ? sorted[0] :
        state.sample_count == 2 ? uint16_t((uint32_t(sorted[0]) + sorted[1]) / 2) : sorted[1];
    out_percent = percent(filtered);
    age_seconds = uint32_t(now - state.last_valid_ms) / 1000;
    return true;
}
}
